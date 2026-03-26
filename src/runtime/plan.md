# Order Book Reconstruction — Runtime Engine Plan

## Context

The data generator streams realistic events (ADD_LIMIT, CANCEL, ADD_MARKET, EXECUTE) but the shared memory only passes aggregate BBO snapshots (`MarketData`). The runtime cannot reconstruct a limit order book from this. We need to:

1. Stream full event data via a SPSC ring buffer in shared memory
2. Build a limit order book on the runtime side
3. Expose clean interfaces for future GUI and Grafana-style dashboards

---

## 1. SPSC Event Ring Buffer in Shared Memory

### Problem
`MarketData` has no `order_id`, per-order `qty`, or per-order `price`. The runtime needs the full `OrderEvent` stream.

### Design

**File**: `src/common/shm_types.hpp`

```cpp
struct alignas(64) ShmOrderEvent {
    uint64_t sequence;        // monotonic; 0 = unwritten
    uint64_t timestamp_ns;
    uint64_t order_id;
    int64_t  price;           // the ORDER's quoted price, fixed-point × PRICE_SCALE
                              // (limit price for ADD_LIMIT/CANCEL, fill price for EXECUTE, 0 for ADD_MARKET)
                              // this is ev.price — NOT the mid price
    int64_t  qty;
    int64_t  qty_remaining;
    uint8_t  type;            // EventType enum
    uint8_t  side;            // Side enum
    uint8_t  _pad[6];
};
static_assert(sizeof(ShmOrderEvent) == 64);

inline constexpr uint32_t EVENT_RING_CAPACITY = 8192; // power of 2
inline constexpr uint32_t EVENT_RING_MASK     = EVENT_RING_CAPACITY - 1;

struct alignas(64) EventRingBuffer {
    alignas(64) std::atomic<uint64_t> head;   // producer-owned
    alignas(64) std::atomic<uint64_t> tail;   // consumer-owned
    alignas(64) ShmOrderEvent slots[EVENT_RING_CAPACITY];
};
```

**Size**: 128B metadata + 64 × 8192 = ~512 KB. Fits within 1 MB SHM.

**Memory ordering (SPSC)**:
- Producer: write slot fields (relaxed), then `head.store(h+1, release)`
- Consumer: `head.load(acquire)`, read slot (relaxed), then `tail.store(t+1, release)`
- Producer checks `head - tail.load(acquire) < CAPACITY` for back-pressure

**Keep `MarketData`**: Yes — burst detection/EMA/accelerator batching depend on it. `MarketData` = lossy latest snapshot (mid price, BBO). `EventRingBuffer` = lossless ordered stream (per-order quoted prices).

**Updated `SharedMemoryBlock`**: Add `alignas(64) EventRingBuffer event_ring;` field. Bump `SHM_VERSION` to 2.

### Data generator changes

**File**: `src/data/main.cpp` — after existing `MarketData` stores, write each event to the ring buffer. The price written to the ring must be `ev.price` (the order's quoted price), NOT `ge.mid`:

```cpp
slot.price = ev.price;  // order's quoted limit/fill price — NOT mid
```

This means:
- ADD_LIMIT: the limit price the order was placed at
- CANCEL: the price of the order being cancelled
- EXECUTE: the price level where the fill occurred
- ADD_MARKET: 0 (market orders have no quoted price)

The existing `MarketData.price` field continues to carry the mid price for EMA/burst detection — these are two separate data paths.

**File**: `src/data/data_generator.cpp` line 186 — EXECUTE events currently have `order_id = 0` because fills are level-aggregate (one EXECUTE per price level, not per order). **Keep this as-is.** The order book will handle EXECUTE as level-aggregate decrements. Individual orders at filled levels become stale in the order map and get cleaned up on subsequent CANCEL attempts (silently ignored if order not found).

---

## 2. Order Book Data Structure

### Design rationale
The runtime is single-threaded and is an event consumer, not a matching engine. No lock-free book internals needed — simple STL containers, atomics only at the SHM boundary (already handled by the ring buffer).

### Files: `src/runtime/order_book.hpp`, `src/runtime/order_book.cpp`

```cpp
struct PriceLevel {
    int64_t  price;        // fixed-point — the quoted price at this level
    int64_t  total_qty;
    uint32_t order_count;
};

struct TrackedOrder {
    uint64_t order_id;
    uint8_t  side;
    int64_t  price;        // the order's quoted limit price
    int64_t  qty;
};

class OrderBook {
public:
    // Event processing
    void on_add_limit(uint64_t order_id, uint8_t side, int64_t price, int64_t qty);
    void on_cancel(uint64_t order_id);
    void on_execute(uint8_t side, int64_t price, int64_t qty, int64_t qty_remaining);

    // Queries
    int64_t  best_bid() const;
    int64_t  best_ask() const;
    int64_t  spread() const;
    double   bid_ask_imbalance(size_t levels = 1) const;
    double   volume_weighted_mid(size_t levels = 3) const;

    // Depth (returns up to max_levels)
    size_t bid_depth(PriceLevel* out, size_t max_levels) const;
    size_t ask_depth(PriceLevel* out, size_t max_levels) const;

    uint64_t total_bid_qty() const;
    uint64_t total_ask_qty() const;

private:
    std::map<int64_t, PriceLevel, std::greater<>> bids_;  // highest first → begin() = best bid
    std::map<int64_t, PriceLevel>                 asks_;  // lowest first  → begin() = best ask
    std::unordered_map<uint64_t, TrackedOrder>    orders_;
};
```

### Event processing logic

- **on_add_limit**: Insert into `orders_` keyed by `order_id`. Find-or-create `PriceLevel` at the order's quoted price, increment qty/count.
- **on_cancel**: Lookup `order_id` in `orders_`. Use the stored quoted price to find the level. Decrement level qty/count. Erase level if qty=0. Erase from `orders_`. Silently ignore if order_id not found (already filled).
- **on_execute**: Level-aggregate: decrement `total_qty` at the fill price level by `qty`. If `qty_remaining == 0`, erase level. Walk `orders_` at that price to remove filled orders (FIFO, decrement `to_fill` until exhausted). This matches the generator's `walk_levels` logic.
- **on_add_market**: No-op. Subsequent EXECUTEs handle fills.

---

## 3. Snapshot Interface (for GUI / Metrics Threads)

### File: `src/runtime/book_snapshot.hpp`

Seqlock-published snapshot — written by poll thread, read by any observer thread.

```cpp
constexpr size_t SNAPSHOT_DEPTH = 10;

struct BookLevel {
    int64_t  price;        // quoted price at this level
    int64_t  total_qty;
    uint32_t order_count;
};

struct alignas(64) BookSnapshot {
    std::atomic<uint64_t> version;  // seqlock: odd = writing, even = stable

    int64_t best_bid, best_ask, spread;
    BookLevel bids[SNAPSHOT_DEPTH];
    BookLevel asks[SNAPSHOT_DEPTH];
    uint32_t bid_level_count, ask_level_count;
    int64_t  total_bid_qty, total_ask_qty;
    double   imbalance;
    double   vwap_mid;
    uint64_t event_sequence;
    uint64_t snapshot_time_ns;
};
```

**Writer** (poll thread): `version.fetch_add(1, release)` → fill fields → `version.fetch_add(1, release)`

**Reader** (GUI/metrics thread): spin-read until two consistent even reads of `version`.

**Throttle**: Publish at most once per 64 events or 1μs, whichever first. During 5ns bursts, this avoids 200M snapshots/sec.

---

## 4. Threading Model

**Recommendation: Inline book updates in the poll loop.** No second thread.

- Book operations (~50-120ns per event) are fast enough for normal mode (50ns inter-arrival).
- During bursts (5ns inter-arrival), the ring buffer absorbs the backlog. The runtime catches up between bursts.
- A second thread adds latency (2 extra cache-line transfers per event) and complexity with no benefit at this stage.
- The existing `MarketData` path for burst detection runs independently — it reads the latest BBO snapshot (mid price), not the ring.

### Modified poll loop structure

```
while (running):
    // PATH 1: MarketData snapshot (existing, unchanged)
    //   → cache, EMA, burst detection, accelerator batching
    //   uses mid price from MarketData.price

    // PATH 2: Event ring (NEW)
    //   drain ring → book.on_add_limit / on_cancel / on_execute
    //   uses per-order quoted prices from ShmOrderEvent.price
    //   throttled snapshot publication

    // Pause only if both paths idle
    if (no new MarketData && ring empty): _mm_pause()
```

---

## 5. Rust vs C++ for Dashboard

### Verdict: Stay with C++ for the MVP. Consider Rust later if the dashboard grows beyond metrics.

**For Prometheus metrics only** (Grafana-style):
- **C++ with `prometheus-cpp`**: One process, one build system, direct `BookSnapshot` access via seqlock, no serialization. `prometheus-cpp` is battle-tested (used by Envoy, gRPC). Adds one dependency.
- **Rust sidecar**: Must replicate all shared structs with `#[repr(C, align(64))]` — error-prone, must stay in sync. Two build systems (CMake + Cargo). Two binaries to deploy. The isolation benefit is real but solvable with CPU pinning.

**When Rust becomes worth it**: If the dashboard grows to interactive order book visualization, WebSocket streaming, REST APIs for historical data — Rust's web ecosystem (`axum`, `tokio`) is genuinely better for that. At that point, consider using `bindgen` to auto-generate Rust bindings from C headers and `static_assert`/`const_assert!` on struct sizes.

**For now**: Add `prometheus-cpp` to CMake, create a metrics thread in the runtime that reads `BookSnapshot` via seqlock and serves `/metrics`.

---

## 6. Implementation Steps

| Step | Files | What |
|------|-------|------|
| 1 | `src/common/shm_types.hpp` | Add `ShmOrderEvent`, `EventRingBuffer`, update `SharedMemoryBlock`, bump version |
| 2 | `src/data/main.cpp` | Write each event to the ring buffer (`ev.price`, not mid) after existing `MarketData` stores |
| 3 | `src/runtime/order_book.hpp`, `order_book.cpp` | `OrderBook` class with maps, order tracking, event handlers |
| 4 | `src/runtime/book_snapshot.hpp` | `BookSnapshot` seqlock struct and publish/read functions |
| 5 | `src/runtime/runtime_engine.hpp/.cpp` | Add `OrderBook` + `BookSnapshot` members, ring consumption in poll loop |
| 6 | `CMakeLists.txt` | Add `order_book.cpp` to `runtime_engine` target |
| 7 | `src/tests/order_book_test.cpp` | Unit tests: known event sequences → verify BBO, depth, imbalance |

## 7. Verification

1. Build all targets
2. Run `data_sim` + `runtime_engine` — runtime should consume ring and maintain book state
3. Print BBO/spread/depth periodically — should match the generator's own `best_bid`/`best_ask` values in the `MarketData` snapshot (within a few events of lag)
4. Unit test: deterministic event sequence → assert `best_bid < best_ask`, depth non-negative, cancel of unknown order is no-op
5. Stress test: sustained burst → confirm ring doesn't overflow (head - tail < 8192)
