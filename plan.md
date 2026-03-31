# 4-Process Engine Restructuring Roadmap

## Why

The engine currently mixes concerns across 3 processes. This restructuring separates data generation, matching, book reconstruction, strategy, and display into clean process boundaries with purpose-built IPC buffers. The result is lower end-to-end latency and a modular architecture where adversary agents, strategy variants, and accelerator offloading can be swapped independently.

## Architecture Overview

```
                    ┌─────────────────────────────────────────────┐
                    │          EXCHANGE (Process 1)                │
                    │                                             │
  ┌──────────┐     │  [Creator]──heap──>[Dispatcher]──┐          │
  │ Strategy  │─────┼──── Buffer 1 (Array of SPSC) ──>[Matching  │
  │(Process 3)│     │  [Adversary*]───────────────────> Engine]   │
  └─────▲─────┘     │                                    │        │
        │           └────────────────────────────────────┼────────┘
        │                                                │
        │                                          Buffer 2 (SPSC)
        │                                                │
        │           ┌───────────────────────────────────-┼────────┐
        │           │       RUNTIME (Process 2)          │        │
        │           │                                    ▼        │
        │           │  [Hot Path]──Buffer 3 (SPSC)──>  (to Strategy)
        │           │      │                                      │
   Buffer 3        │      └──local seqlock──>[Snapshotter]       │
                    │                              │               │
                    └──────────────────────────────┼───────────────┘
                                                   │
                                             Buffer 4 (Seqlock)
                                                   │
                                                   ▼
                                          ┌──────────────┐
                                          │  DASHBOARD    │
                                          │ (Process 4)   │
                                          └──────────────┘
```

## CPU Pinning Map (6C/12T)

| Core | Thread | Notes |
|------|--------|-------|
| 0 | Matching Engine | Hottest — polls all input rings |
| 1 | Data Dispatcher | Clock-driven, sleeps between events |
| 2 | Runtime Hot Path | Book reconstruction + strategy feed |
| 3 | Strategy | Decision loop |
| 4 | Data Creator | Batch precompute, not latency-critical |
| 5 | Snapshotter + Dashboard | Cool threads, can share |

---

## Phase 1: Foundation — SHM Layout, Utilities, Hugepages

**Goal**: New shared memory layout with all 4 buffers, hugepage backing, and reusable utilities.

### 1a. `src/common/shm_types.hpp` — redesign

- Bump `SHM_VERSION` to 4, set `SHM_SIZE` to `16 * 1024 * 1024` (16 MB = 8 hugepages)
- Add `source_id` field to `ShmOrderEvent` (repurpose 1 byte of `_pad[6]` → `source_id` + `_pad[5]`). Values: 0=dispatcher, 1=strategy, 2+=adversary.
- Define **Buffer 1** — `ExchangeInputArray`:
  - `MAX_EXCHANGE_PRODUCERS = 4` (dispatcher + strategy + 2 adversary slots)
  - `EXCHANGE_RING_CAPACITY = 4096` (power of 2)
  - `ExchangeInputRing` — same SPSC pattern as `EventRingBuffer` (atomic head/tail on separate cache lines, slots array)
  - `ExchangeInputArray` — `active_count` + `rings[MAX_EXCHANGE_PRODUCERS]`
  - Array-of-SPSC chosen over MPSC because each producer writes to its own ring with zero contention. MPSC requires CAS which causes cache-line bouncing. The matching engine polls with priority ordering.
- Define **Buffer 3** — `StrategyRingBuffer`:
  - `StrategyTick` (128 bytes, 2 cache lines): sequence, timestamp_ns, enqueue_tsc, best_bid, best_ask, mid, ema, imbalance (double), tick_rate (double), last_event_type, last_event_side, in_burst, pad
  - `STRATEGY_RING_CAPACITY = 4096`
  - `StrategyRingBuffer` — same SPSC pattern with `StrategyTick` slots
- **Buffer 2** reuses existing `EventRingBuffer` (8192 slots of `ShmOrderEvent`)
- **Buffer 4** is the existing `BookSnapshot` with seqlock (move into `SharedMemoryBlock`)
- New `SharedMemoryBlock`:
  ```
  ShmHeader, ExchangeInputArray, EventRingBuffer (market_data_feed),
  StrategyRingBuffer (strategy_feed), BookSnapshot (dashboard_snapshot),
  AcceleratorBatch, AcceleratorSignal
  ```
- Keep all `static_assert` checks, add one for `sizeof(SharedMemoryBlock) <= SHM_SIZE`

### 1b. `src/common/shm_manager.hpp/cpp` — hugepage support

- In `create()`: change the `mmap` call to use `MAP_SHARED | MAP_HUGETLB | MAP_HUGE_2MB`
  - **Fallback**: If hugepage mmap fails (`ENOMEM` or `EINVAL`), retry without `MAP_HUGETLB` and print a warning. This keeps the engine runnable on systems without hugepages configured.
  - Use `madvise(addr, size, MADV_HUGEPAGE)` as a secondary hint after mmap (for transparent hugepages fallback)
- In `open()`: same hugepage flags on the consumer side mmap
- Add `#include <sys/mman.h>` for `MAP_HUGETLB` / `MAP_HUGE_2MB` (should already be there for `mmap`)
- SHM_SIZE of 16 MB is exactly 8 hugepages (2 MB each) — clean alignment
- **System setup note**: The user needs to ensure hugepages are available:
  ```bash
  echo 16 > /proc/sys/vm/nr_hugepages   # reserve 16 x 2MB = 32MB
  # or for persistence: add "vm.nr_hugepages=16" to /etc/sysctl.conf
  ```
  Also verify hugetlbfs is mounted (often at `/dev/hugepages`). On WSL2, transparent hugepages should work via `madvise`.

### 1c. `src/common/spsc_queue.hpp` — new file

Templated process-local SPSC queue for Creator→Dispatcher heap buffer:
```cpp
template<typename T, uint32_t Capacity>
struct alignas(64) SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0);
    static constexpr uint32_t MASK = Capacity - 1;
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) T slots[Capacity];
    bool try_push(const T& item) noexcept;
    bool try_pop(T& out) noexcept;
    uint64_t size() const noexcept;
};
```
Use the same acquire/release memory ordering as the SHM rings. Capacity 16384 for the Creator→Dispatcher buffer.

Allocate on the heap (or via hugepage-backed anonymous mmap for the local buffer too — optional optimization). The key is head/tail on separate cache lines.

### 1d. `src/common/cpu_pin.hpp` — new file

```cpp
#include <pthread.h>
#include <sched.h>

inline bool pin_to_core(int core_id) noexcept {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}
```

### 1e. Verification

- Everything compiles (no new executables yet, just the library/headers)
- `static_assert` checks pass for new struct sizes and alignment
- Print `sizeof(SharedMemoryBlock)` to confirm it fits in 16 MB

---

## Phase 2: Exchange Process

**Goal**: Replace `data_sim` with the Exchange process (3 threads: Creator, Dispatcher, Matching Engine).

### 2a. `src/exchange/data_creator.hpp` — new file (header-only)

Thin wrapper: instantiate `DataGenerator`, call `generator.next()` in a tight loop, push `GeneratedEvent` into the local `SpscQueue`. Does NOT respect `wait_ps` — just precomputes as fast as possible. Call `pin_to_core(4)` at thread start.

### 2b. `src/exchange/data_dispatcher.hpp` — new file (header-only)

Pop `GeneratedEvent` from local queue, convert to `ShmOrderEvent`, write to `exchange_input.rings[0]` with `source_id=0`. Respect `wait_ps` timing using the sleep/spin logic from current `src/data/main.cpp` (sleep >50us, busy-wait <50us, clamp to prevent time debt).

**Prefetch optimization**: During the wait interval, before sleeping:
```cpp
auto next_idx = (local_tail + 1) & SpscQueue::MASK;
__builtin_prefetch(&local_queue.slots[next_idx], 0, 3);  // read, L1 hint
```
Call `pin_to_core(1)` at thread start.

### 2c. `src/exchange/matching_engine.hpp` + `matching_engine.cpp` — new files

`MatchingEngine` class holding:
- An `OrderBook` instance (the "true" exchange book)
- Pointer to `ExchangeInputArray` (Buffer 1) and `EventRingBuffer` (Buffer 2, market_data_feed)
- Per-ring tail trackers
- A sequence counter for Buffer 2 output

**Poll loop** (priority-based):
1. Drain ring 0 (dispatcher) — all available events
2. Drain ring 1 (strategy) — all available events
3. Drain rings 2..active_count-1 (adversaries) — round-robin, 1 event each

**Processing logic per event**:
- **From ring 0 (dispatcher)**: Events arrive pre-matched (DataGenerator already emits EXECUTE, ADD_LIMIT, CANCEL in dependency order). Apply directly to internal book and forward unchanged to Buffer 2.
- **From rings 1+ (strategy/adversary)**: Raw orders needing actual matching:
  - `ADD_LIMIT`: Check if it crosses the book. If yes, walk opposing side generating EXECUTE events to Buffer 2, then add residual as passive. If no, add passively and emit ADD_LIMIT to Buffer 2.
  - `ADD_MARKET`: Walk opposing side, emit EXECUTE events to Buffer 2.
  - `CANCEL`: Remove from book, emit CANCEL to Buffer 2.

**Prefetch**: After consuming a slot from any ring:
```cpp
__builtin_prefetch(&ring.slots[(tail + 1) & MASK], 0, 3);
```

Call `pin_to_core(0)` at thread start.

### 2d. `src/exchange/main.cpp` — new file

- Signal handling (SIGINT/SIGTERM)
- Create SHM (owner process)
- Config loading (reuse interactive picker or CLI arg from current `src/data/main.cpp`)
- Set `exchange_input.active_count = 2` (dispatcher + strategy; adversaries added later)
- Spawn 3 threads: Creator, Dispatcher, Matching Engine
- Join on shutdown

### 2e. Delete `src/data/main.cpp`

All its logic is now split across Creator, Dispatcher, and exchange/main.cpp.

### 2f. Verification

- Build just the exchange target
- Run exchange alone, verify it creates SHM and events flow from Creator → Dispatcher → Matching Engine → Buffer 2
- Check CPU affinity with `taskset -pc <pid>` or `cat /proc/<tid>/status | grep Cpus_allowed`
- Verify hugepage allocation: `cat /proc/meminfo | grep HugePages`

---

## Phase 3: Runtime Refactor

**Goal**: Refactor runtime_engine into 2 threads consuming from Buffer 2 and writing to Buffer 3 + Buffer 4.

### 3a. `src/runtime/runtime_engine.hpp/cpp` — modify

- Change the consumer to read from `market_data_feed` (Buffer 2) instead of the old `event_ring`
- After processing each event and deriving market state (book update, BurstDetector, TickProcessor — all unchanged), construct a `StrategyTick` and write it to `strategy_feed` (Buffer 3)
- Write to a **process-local** `BookSnapshot` via seqlock every 64 events (same as current `publish_snapshot`)
- **Remove** all accelerator batch/signal logic (flush_batch, pending_batch_, etc.) — strategy handles this now
- **Remove** `AcceleratorTick`/`AcceleratorBatch` member variables
- **Prefetch** on Buffer 2: `__builtin_prefetch(&ring.slots[(tail+1) & MASK], 0, 3)` after consuming each slot
- Call `pin_to_core(2)` at start of `run()`

### 3b. `src/runtime/main.cpp` — rewrite

Two threads:
- **Hot Path thread**: Runs `RuntimeEngine::run()` — consumes Buffer 2, updates local book, writes to Buffer 3 + local seqlock snapshot
- **Snapshotter thread**: Reads the local `BookSnapshot` via `snapshot_read()`, copies to SHM `dashboard_snapshot` via seqlock write. Run at ~60Hz (sleep 16ms between iterations, or spin on local snapshot version change). Call `pin_to_core(5)`.
- Main thread: open SHM, spawn both threads, join on shutdown
- **Remove** display thread spawning and all imgui/implot references

### 3c. `src/runtime/book_snapshot.hpp` — minor update

The `ring_occupancy` field currently reads from the old single event_ring. Update `publish_snapshot` to compute occupancy from `market_data_feed` (Buffer 2) instead. Same formula: `(head - tail) / capacity`.

### 3d. Verification

- Start exchange, then runtime
- Verify runtime attaches to SHM, consumes from Buffer 2, writes to Buffer 3
- Print sequence numbers at each hop to confirm data flows
- Check that local seqlock snapshot updates correctly

---

## Phase 4: Strategy Process

**Goal**: Implement the strategy process consuming Buffer 3 and writing orders to Buffer 1.

### 4a. `src/strategy/main.cpp` — new file

- Open SHM
- Consume from `strategy_feed` (Buffer 3)
- Measure queue backup: `head.load(relaxed) - local_tail`. If backup exceeds threshold, flag for accelerator offloading (retain the `AcceleratorBatch`/`AcceleratorSignal` logic moved from RuntimeEngine)
- **Stub strategy logic**: Initially just a pass-through or simple mean-reversion signal. When the strategy decides to place an order, write `ShmOrderEvent` to `exchange_input.rings[1]` with `source_id=1`
- **Prefetch**: `__builtin_prefetch(&strategy_feed.slots[(tail+1) & MASK], 0, 3)` after consuming
- Call `pin_to_core(3)` at start

### 4b. Verification

- Start exchange → runtime → strategy
- Verify strategy consumes ticks from Buffer 3
- Have the stub place a test order, verify it appears in Buffer 1 ring 1
- Verify matching engine processes the strategy order and result appears in Buffer 2

---

## Phase 5: Dashboard Process

**Goal**: Extract the display into its own process reading from Buffer 4.

### 5a. `src/dashboard/main.cpp` — new file

- Open SHM (read-only is ideal, but seqlock reads via mmap work with `PROT_READ | PROT_WRITE` since the atomic version counter needs load)
- Get a reference to `dashboard_snapshot` from the `SharedMemoryBlock`
- Instantiate `DisplayThread` and call `display.run(snapshot, running)` — the existing display code reads from `BookSnapshot` via `snapshot_read()` and the protocol is identical whether the snapshot is local or mmap'd
- Call `pin_to_core(5)` (shares core with Snapshotter)
- Signal handling for clean shutdown

### 5b. `src/runtime/display_thread.cpp` — no changes

The display code is agnostic to where the `BookSnapshot` lives. It calls `snapshot_read()` which works on any memory-mapped `BookSnapshot`.

### 5c. Delete `src/accelerator/main.cpp`

Replaced by strategy process.

### 5d. Verification

- Start all 4 processes: exchange → runtime → strategy → dashboard
- Verify the GUI renders with live data
- Verify orderbook, plots, latency metrics all display correctly

---

## Phase 6: Build System

**Goal**: Update CMakeLists.txt for the 4 new targets.

### 6a. `CMakeLists.txt` — modify

- Add `order_book_lib` static library: `src/runtime/order_book.cpp` (shared between exchange and runtime)
- **Remove** targets: `data_sim`, `runtime_engine`, `accelerator_sim`
- **Add** target `exchange`: sources `src/exchange/main.cpp`, `src/exchange/matching_engine.cpp`. Link: `data_generator_lib`, `order_book_lib`, `common`, Threads, rt
- **Add** target `runtime`: sources `src/runtime/main.cpp`, `src/runtime/runtime_engine.cpp`. Link: `order_book_lib`, `common`, Threads, rt. **No imgui/implot.**
- **Add** target `strategy`: source `src/strategy/main.cpp`. Link: `common`, Threads, rt
- **Add** target `dashboard`: sources `src/dashboard/main.cpp`, `src/runtime/display_thread.cpp`. Link: `common`, imgui_lib, implot_lib, Threads, rt. Define `JETBRAINS_MONO_FONT`.
- Keep test targets unchanged

Note: You can update CMakeLists.txt incrementally as you build each phase, or do it all at the end. Incremental is recommended so you can compile-test each phase.

---

## Phase 7: End-to-End Validation

1. Reserve hugepages: `echo 16 | sudo tee /proc/sys/vm/nr_hugepages`
2. Build all: `cmake --build build`
3. Launch in order: `./build/exchange configs/btc_spot.json`, then `./build/runtime`, `./build/strategy`, `./build/dashboard`
4. Verify hugepages consumed: `grep HugePages /proc/meminfo`
5. Verify CPU pinning: `for pid in $(pgrep -f 'exchange|runtime|strategy|dashboard'); do echo "$pid: $(taskset -pc $pid)"; done`
6. Measure end-to-end latency: TSC at dispatcher enqueue → TSC at strategy consumption
7. Test strategy order flow: strategy places order → matching engine matches → result in Buffer 2 → runtime reconstructs
8. Stress test: use a fast config (e.g., `btc_perp.json` with 10us intervals), watch queue depths and latency percentiles

---

## Latency Optimizations Summary

| Optimization | Where | Details |
|---|---|---|
| Hugepages (2MB) | ShmManager mmap | Eliminates TLB misses across 16MB SHM. Fallback to regular pages if unavailable. |
| `__builtin_prefetch` | Dispatcher wait, Matching Engine poll, Runtime consume, Strategy consume | Prefetch next ring slot into L1 during idle cycles |
| CPU pinning | All 6 threads | Dedicated physical cores for hot threads, eliminates context-switch jitter |
| Array-of-SPSC | Buffer 1 | Zero contention between producers (no CAS), priority polling by consumer |
| Cache-line separation | All ring head/tail | Already in place (`alignas(64)`), prevents false sharing |
| Seqlock | Buffer 4 | Lock-free snapshot reads, no blocking on hot path |

## Files Reference

### New files
- `src/common/spsc_queue.hpp`, `src/common/cpu_pin.hpp`
- `src/exchange/main.cpp`, `src/exchange/matching_engine.hpp`, `src/exchange/matching_engine.cpp`
- `src/exchange/data_creator.hpp`, `src/exchange/data_dispatcher.hpp`
- `src/strategy/main.cpp`
- `src/dashboard/main.cpp`

### Modified files
- `src/common/shm_types.hpp` (major — new layout)
- `src/common/shm_manager.hpp/cpp` (hugepages)
- `src/runtime/runtime_engine.hpp/cpp` (consume Buffer 2, write Buffer 3, remove accelerator)
- `src/runtime/main.cpp` (2 threads, no display)
- `src/runtime/book_snapshot.hpp` (ring_occupancy source change)
- `CMakeLists.txt` (4 new targets)

### Deleted files
- `src/data/main.cpp` → replaced by `src/exchange/main.cpp`
- `src/accelerator/main.cpp` → replaced by `src/strategy/main.cpp`

### Unchanged files
- `src/data/data_generator.hpp/cpp`, `src/data/config.hpp`, `src/data/json.hpp`
- `src/runtime/order_book.hpp/cpp`
- `src/runtime/display_thread.hpp/cpp`
- `src/common/engine_types.hpp`, `src/common/spin_pause.hpp`
- `configs/*.json`
