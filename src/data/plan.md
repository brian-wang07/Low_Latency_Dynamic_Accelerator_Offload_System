# Lock-Free Limit Order Book — Full System Overhaul Spec

## Overview

This document specifies a complete overhaul from a mid-price stream to a structurally faithful synthetic market microstructure system. The end state is an ITCH-style event stream driving a lock-free limit order book, with mid, BBO, spread, and depth as emergent outputs — not inputs.

The system has three layers:
1. **Generative model** — produces a stream of order events from a calibrated stochastic process
2. **Lock-free book** — consumes the event stream and maintains book state with atomic operations
3. **Derived outputs** — BBO, mid, spread, depth snapshot published via seqlock

---

## Part 1 — Generative Model

### 1.1 Mid Price Process

Use an Ornstein-Uhlenbeck process discretised with Euler-Maruyama. This gives mean reversion within a session while preserving short-term random walk character.

```
μ(0) = μ_0
μ(t + Δt) = μ(t) + κ(μ̄ - μ(t))Δt + σ√Δt · Z,   Z ~ N(0,1)

Parameters:
  μ_0  = initial mid price (e.g. 100.00)
  κ    ∈ [0.5, 5.0]       reversion speed
  μ̄   = μ_0               long-run mean
  σ    ∈ [0.01, 0.05]/√s   volatility in tick units
```

After each step, snap μ to the nearest half-tick so BBO stays on-grid:

```python
mu = round(mu / (0.5 * tick)) * (0.5 * tick)
```

Δt here is the inter-event time drawn from the Hawkes process (Layer 1.3), not a fixed wall-clock step.

### 1.2 Spread Process

CIR process — keeps spread positive, mean-reverting, and conditionally heteroskedastic:

```
s(0) = s_0 = 2 ticks
s(t + Δt) = s(t) + κ_s(θ_s - s(t))Δt + ξ√(s(t))√Δt · Z_s

Parameters:
  κ_s  ∈ [1.0, 10.0]   reversion speed
  θ_s  = 2 ticks        long-run mean
  ξ    ∈ [0.1, 0.5]     spread volatility
  Z_s ~ N(0,1), independent of Z in mid process
  floor: s ≥ 1 tick (hard reflect at lower bound)
```

BBO derived from mid and spread:

```python
best_bid = floor((mu - s / 2) / tick) * tick
best_ask = best_bid + round(s / tick) * tick
```

### 1.3 Event Clock — Hawkes Process

Order arrivals are not equally spaced. Use a mutually exciting Hawkes process with one intensity per side. This captures the empirically observed clustering of order flow.

```
λ_bid(t) = μ_0 + Σ_{t_i < t, side=bid} α · exp(-β(t - t_i))
λ_ask(t) = μ_0 + Σ_{t_i < t, side=ask} α · exp(-β(t - t_i))

Parameters:
  μ_0  ∈ [5, 50] events/sec   baseline arrival rate per side
  α    ∈ [0.3, 0.8]            excitation magnitude (α/β < 1 for stationarity)
  β    ∈ [1.0, 10.0]           decay rate
```

Simulation via Ogata thinning algorithm:

```python
def simulate_hawkes(T, mu0, alpha, beta):
    t, events = 0.0, []
    lam_bar = mu0  # upper bound on intensity
    while t < T:
        dt = -log(uniform()) / lam_bar
        t += dt
        lam_t = mu0 + alpha * sum(exp(-beta * (t - s)) for s in events)
        if uniform() < lam_t / lam_bar:
            events.append(t)
            lam_bar = lam_t + alpha  # update upper bound
        else:
            lam_bar = lam_t
    return events
```

**Fallback**: If Hawkes complexity is not needed, use homogeneous Poisson:
```
inter_arrival ~ Exponential(λ),   λ = 20/sec per side
```

### 1.4 Event Type Sampling

At each arrival, draw the event type:

```
P(ADD_LIMIT)    = 0.70
P(CANCEL)       = 0.20   — of a uniformly selected live order
P(ADD_MARKET)   = 0.08
P(MODIFY)       = 0.02   — implemented as CANCEL + ADD

Guard: if no live orders exist when CANCEL or MODIFY is drawn, resample as ADD_LIMIT.
```

Additionally, each live limit order carries an independent exponential lifetime to produce the empirical ~90% cancel rate:

```
lifetime ~ Exponential(γ),   γ = 10 × μ_0
```

When a lifetime expires, emit a CANCEL event for that order.

### 1.5 Price Placement (ADD_LIMIT)

Sample price offset δ from BBO in ticks. Geometric distribution with truncation:

```
δ ~ Geometric(p),   p = 0.35
    truncated at D = 10 ticks (rejection sample or truncate-and-renormalise)

# Bid order
price_bid = best_bid - δ × tick

# Ask order  
price_ask = best_ask + δ × tick
```

δ = 0 means at-the-quote. ~35% of orders land there; the rest are passive.

### 1.6 Order Quantity

Log-normal, rounded to lot size:

```
qty = max(1, round(exp(μ_q + σ_q · Z)))   Z ~ N(0,1)
qty = round(qty / lot_size) * lot_size,   minimum 1 lot

Parameters:
  μ_q     = log(100)   median ~100 shares
  σ_q     = 0.8        heavy right tail
  lot_size = 100        US equities; 1 for crypto
```

### 1.7 Price Impact Feedback

After a market order fills N ticks of depth, apply a feedback nudge to μ:

```
Δμ = sign(order.side) × impact_coeff × fill_qty / avg_depth_at_bbo
impact_coeff ∈ [0.01, 0.1] ticks per lot
```

This closes the loop between fills and the mid process so the book doesn't diverge from a random walk.

### 1.8 Output — Event Stream Format

The generative model emits a flat stream of order events. No BBO, mid, or spread in the stream — those are derived by the consumer.

```cpp
enum class EventType : uint8_t {
    ADD_LIMIT,
    ADD_MARKET,
    CANCEL,
    EXECUTE,
    TRADE       // non-book print (crosses, odd lots)
};

enum class Side : uint8_t { BID, ASK };

struct OrderEvent {
    uint64_t  timestamp_ns;     // nanoseconds since session open
    uint64_t  order_id;         // monotonically increasing
    EventType type;
    Side      side;
    int64_t   price;            // fixed-point: price × 10^6; 0 for market orders
    int64_t   qty;              // shares / lots
    int64_t   qty_remaining;    // after partial fill; equals qty for ADD events
};
```

---

## Part 2 — Lock-Free Limit Order Book

### 2.1 Core Data Structures

```cpp
enum class OrderState : uint8_t { Active, Cancelled, Filled, PartialFill };
enum class OrderKind  : uint8_t { Limit, Market };
enum class Side       : uint8_t { Bid, Ask };

struct Order {
    uint64_t             order_id;
    Side                 side;
    OrderKind            kind;
    int64_t              price;
    std::atomic<int64_t> qty;
    std::atomic<Order*>  next;          // intrusive linked list within PriceLevel
    std::atomic<OrderState> state;
};

struct PriceLevel {
    int64_t              price;         // fixed-point
    std::atomic<int64_t> total_qty;     // aggregate; updated via fetch_add/sub
    std::atomic<Order*>  head;          // head of intrusive order list
};
```

### 2.2 Price Index

Two options. Choose based on whether tick size and price range are bounded.

**Option A — Bounded flat array (preferred for equities/crypto):**

```cpp
// Pre-allocate a fixed window around the initial mid price
static constexpr int LEVELS = 2048;   // ticks on each side of mid

struct PriceLevelArray {
    int64_t     base_price;            // price at index 0
    int64_t     tick_size;
    PriceLevel  levels[LEVELS];

    int index(int64_t price) const {
        return (int)((price - base_price) / tick_size);
    }
    PriceLevel* at(int64_t price) {
        int i = index(price);
        if (i < 0 || i >= LEVELS) return nullptr;
        return &levels[i];
    }
};

OrderBook {
    PriceLevelArray bids;   // same array type; best bid = highest occupied index
    PriceLevelArray asks;   // best ask = lowest occupied index
    std::atomic<int> best_bid_idx;
    std::atomic<int> best_ask_idx;
};
```

O(1) access, no ABA problem, no memory reclamation for levels. Recenter the window if mid drifts beyond the bounds (copy live levels, CAS the base_price).

**Option B — Lock-free skip list (unbounded price range):**

Use the Harris-Herlihy-Shavit split-reference technique. Pack `(pointer, mark_bit)` into a 64-bit atomic to support logical deletion before physical unlinking. Use `std::atomic<uintptr_t>` with the low bit as the mark.

```cpp
struct SkipNode {
    int64_t price;
    PriceLevel level;
    std::atomic<uintptr_t> next[MAX_HEIGHT];  // tagged pointer: ptr | deleted_flag
};
```

Only needed if the price range is unbounded or tick size is variable.

### 2.3 Order Map

For cancel and modify lookups by order_id:

```
lock-free hash map: uint64_t → Order*
```

Use open-addressing with linear probing and CAS on slot state, or an existing implementation (`folly::AtomicHashMap`, `libcuckoo`, `tbb::concurrent_hash_map`).

### 2.4 NEW Order Processing

```
ADD_LIMIT:
  1. Allocate Order node (from epoch-protected pool — see 2.7)
  2. Fetch or initialise PriceLevel at price p
  3. CAS-loop: prepend Order to level.head (intrusive list)
  4. fetch_add(level.total_qty, qty, memory_order_release)
  5. Insert (order_id → Order*) into order_map
  6. Check crossing:
       bid: price >= best_ask → trigger matching
       ask: price <= best_bid → trigger matching

ADD_MARKET:
  1. Walk opposite side from best level
  2. For each level: CAS order.state Active→Filling, deduct qty
  3. Advance to next level if total_qty exhausted
  4. Apply price impact feedback to μ
  5. Emit EXECUTE events for each fill
```

### 2.5 CANCEL Processing

The CAS on `order.state` is the linearisation point. The order is logically dead the moment that CAS succeeds, even before total_qty is updated.

```
CANCEL:
  1. Lookup Order* in order_map via order_id
  2. CAS order.state: Active → Cancelled     ← linearisation point
     If CAS fails (already Filled/Cancelled): discard, done
  3. fetch_sub(level.total_qty, order.qty, memory_order_release)
  4. Do NOT physically unlink from list here
  5. Physical unlink deferred to EBR reclamation (see 2.7)
```

Physical unlinking during list traversal: when walking the list for matching, skip nodes with `state == Cancelled` or `state == Filled` and lazily unlink them (CAS the predecessor's next pointer).

### 2.6 MODIFY Processing

Always implement as CANCEL + ADD. The only exception is a same-price quantity reduction, which can be done in-place with a `fetch_sub` while preserving queue position.

```
MODIFY (price change or qty increase):
  1. CANCEL old order (CAS state, fetch_sub qty)
  2. ADD_LIMIT new order at new price/qty
  — Queue position is lost; order goes to back of new level

MODIFY (same price, qty decrease — optional optimisation):
  1. CAS-loop: fetch_sub(order.qty, delta) while state == Active
  2. fetch_sub(level.total_qty, delta)
  — Queue position preserved
```

### 2.7 Memory Reclamation — Epoch-Based Reclamation (EBR)

Cancelled and filled Order nodes cannot be freed immediately — other threads may hold pointers to them during list traversal.

**Three-epoch scheme:**

```cpp
struct EbrDomain {
    std::atomic<uint64_t>  global_epoch{0};  // 0, 1, or 2 mod 3
    thread_local uint64_t  local_epoch;
    thread_local bool      in_critical;
    thread_local std::vector<Order*> limbo[3];  // per-epoch retired list
};

// On thread entry to book operation:
void enter() {
    local_epoch = global_epoch.load(acquire);
    in_critical = true;
}

// On thread exit:
void exit() {
    in_critical = false;
}

// On retiring a node (after CAS on state succeeds):
void retire(Order* p) {
    limbo[global_epoch % 3].push_back(p);
    try_advance();
}

// Advance global epoch if all threads have observed current epoch:
void try_advance() {
    uint64_t e = global_epoch.load();
    bool all_past = true;
    for (auto& t : registered_threads)
        if (t.in_critical && t.local_epoch != e) { all_past = false; break; }
    if (all_past) {
        global_epoch.fetch_add(1);
        // free limbo[(e - 1) % 3] — safe now
        for (Order* p : limbo[(e - 1) % 3]) pool.free(p);
        limbo[(e - 1) % 3].clear();
    }
}
```

For a simpler alternative with higher per-pointer overhead, use **hazard pointers** (`std::hazard_pointer` in C++26, or `folly::hazptr`).

### 2.8 Best Bid/Ask Maintenance

```cpp
std::atomic<int> best_bid_idx;   // index into bids array
std::atomic<int> best_ask_idx;   // index into asks array
```

Update lazily — do not CAS on every fill. Instead:

- After a fill exhausts a level, try CAS `best_bid_idx` or `best_ask_idx` to the next occupied level.
- Readers that observe an empty level at the stored best index scan forward until they find a non-empty level.

This avoids a hot atomic write on every fill while keeping stale reads bounded.

### 2.9 Memory Ordering Summary

| Operation | Ordering | Reason |
|---|---|---|
| CAS on `order.state` | `acq_rel` | Linearisation point; must be visible before/after |
| `fetch_add` on `total_qty` (ADD) | `release` | Quantity visible to readers after insertion |
| `fetch_sub` on `total_qty` (CANCEL/FILL) | `release` | Quantity reduction visible to readers |
| Read of `total_qty` for matching | `acquire` | Pairs with release writes |
| `best_bid_idx` / `best_ask_idx` reads | `acquire` | Must see latest level state |
| Statistical counters (fill count, etc.) | `relaxed` | No synchronisation required |

---

## Part 3 — Derived Outputs

### 3.1 Depth Snapshot via Seqlock

Depth snapshots are read-mostly and tolerate bounded staleness. Seqlock is the standard approach in exchange infra.

```cpp
struct DepthSnapshot {
    std::atomic<uint64_t> seq{0};   // odd = writer active; even = stable
    struct Level { int64_t price; int64_t qty; };
    Level bids[10];
    Level asks[10];
};

// Writer (called after each book mutation):
void publish(DepthSnapshot& snap) {
    snap.seq.fetch_add(1, release);   // seq → odd
    // write bids and asks arrays
    snap.seq.fetch_add(1, release);   // seq → even
}

// Reader:
void read(DepthSnapshot& snap, Level* out_bids, Level* out_asks) {
    uint64_t s1, s2;
    do {
        s1 = snap.seq.load(acquire);
        if (s1 & 1) continue;         // writer active, spin
        memcpy(out_bids, snap.bids, sizeof(snap.bids));
        memcpy(out_asks, snap.asks, sizeof(snap.asks));
        s2 = snap.seq.load(acquire);
    } while (s1 != s2);
}
```

### 3.2 Derived Quantities

All of these are computed by the consumer from the snapshot or live book state. None are stored in the book itself.

```
best_bid  = snap.bids[0].price
best_ask  = snap.asks[0].price
mid       = (best_bid + best_ask) / 2.0
spread    = best_ask - best_bid
depth_bid = Σ snap.bids[i].qty   for i in 0..N
depth_ask = Σ snap.asks[i].qty   for i in 0..N
```

---

## Part 4 — Parameter Reference

```
Symbol          Value / Range           Description
──────────────────────────────────────────────────────────────
μ_0             instrument-dependent    initial mid price
κ               0.5 – 5.0               mid OU reversion speed
σ               0.01 – 0.05 / √s        mid volatility (tick units)
κ_s             1.0 – 10.0              spread reversion speed
θ_s             2 ticks                 long-run spread
ξ               0.1 – 0.5               spread vol
tick            instrument-dependent    minimum price increment
μ_bid/ask       5 – 50 /s               baseline Hawkes arrival rate per side
α               0.3 – 0.8               Hawkes excitation (α/β < 1 required)
β               1.0 – 10.0              Hawkes decay rate
γ               10 × μ_0                per-order cancel rate (exponential lifetime)
p_at_quote      0.35                    P(order placed at BBO)
D               10 ticks                max price offset from BBO
μ_q             log(100)                log-mean order size (shares)
σ_q             0.8                     log-std order size
lot_size        100 (equities), 1 (crypto)  rounding unit
impact_coeff    0.01 – 0.1              price impact per lot filled
LEVELS          2048                    price level array half-width (ticks)
depth_levels    10                      levels published per side in snapshot
```

---

## Part 5 — Implementation Order

Work in this sequence to keep each step independently testable.

**Step 1 — Event emitter**
Implement the generative model as a standalone component that writes `OrderEvent` records to a ring buffer or file. Start with homogeneous Poisson arrival, OU mid, CIR spread, geometric price placement, log-normal qty. Verify that the cancel rate converges to ~90% and depth at BBO is in the 200–2000 share range.

**Step 2 — Single-threaded book**
Build the book with a `std::mutex` first. Implement ADD, CANCEL, EXECUTE, and the seqlock depth publisher. Run the event emitter into this book and assert BBO consistency: `best_bid < best_ask` always, total depth matches sum of level quantities.

**Step 3 — Lock-free conversion**
Replace the mutex with atomics. Implement CAS-loop list prepend, atomic `total_qty` updates, and the lazy best-bid/ask scan. Run a multi-threaded stress test: N writer threads feeding events, M reader threads consuming depth snapshots. Assert no torn reads from the seqlock.

**Step 4 — EBR**
Add epoch-based reclamation for cancelled Order nodes. Verify with ASAN and valgrind that no nodes are freed while referenced, and no memory is leaked under sustained cancel load.

**Step 5 — Hawkes arrival**
Replace Poisson with the Hawkes process (Ogata thinning). Measure inter-arrival time distributions and verify autocorrelation in order flow matches the expected clustering.

**Step 6 — Price impact feedback**
Wire the EXECUTE handler back to the mid process. Verify that large market orders produce short-term adverse price movement in the mid, and that mid reverts over the κ timescale.

---

## Part 6 — Calibration Targets

Use these to validate that your synthetic data is structurally faithful to real ITCH/XDP feeds:

| Statistic | Target |
|---|---|
| Spread at BBO | 1–3 ticks (liquid large-cap) |
| Depth at BBO per side | 200–2000 shares |
| Cancel rate (lifetime) | ≥ 90% of limit orders |
| Market order fraction | 8–20% of total events |
| Order arrival autocorrelation | Decaying positive (Hawkes signature) |
| Mid price autocorrelation | Near zero at 1-event lag (efficient market) |
| Spread autocorrelation | Positive, decaying over ~10–50 events |

If your cancel rate is too low, increase γ. If depth is too thin, decrease γ or increase μ_0. If spread is too volatile, decrease ξ or increase κ_s.