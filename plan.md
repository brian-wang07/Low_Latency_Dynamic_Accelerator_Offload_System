# Low-Latency Engine — Implementation Roadmap

## Project Overview

A kernel-aware userspace system that synthesizes market data via a random walk, streams it
zero-copy over shared memory IPC, and dynamically offloads computation to a separate
accelerator process during microbursts. The long-term target is a market-making strategy
backed by full order-book depth, with the accelerator eventually emulated as a PCIe device
via VFIO rather than shared memory.

Three permanent processes:
- **data_sim** — random-walk price generator with configurable burst behaviour
- **runtime_engine** — the hot path: polls data, tracks state, decides when to offload
- **accelerator_sim** — receives batches from the runtime, processes them, signals results back

---

## Phase 0 — Foundation (complete)

The scaffolding is in place:

- `SharedMemoryBlock` with three cache-line-isolated regions:
  `latest_market_data`, `data_to_accelerator`, `accelerator_signal`
- `ShmManager`: POSIX shm create/open/unlink with RAII ownership
- `DataGenerator`: Gaussian random walk with configurable burst probability, burst length,
  volatility multiplier, and jitter
- `RuntimeEngine`: spinning poll loop (`_mm_pause`), `RingBuffer<CachedTick, 1024>`,
  `TickProcessor` (EMA + tick-rate window), prints every N ticks
- Correct producer/consumer memory ordering:
  stores — price/timestamp relaxed then seq release;
  loads — seq acquire then price/timestamp relaxed

---

## Phase 1 — MVP: Microburst Detection and Accelerator Offload

**Goal:** The runtime detects when a microburst is occurring (tick arrival rate exceeds a
threshold), routes ticks to the accelerator during the burst, and emits a clear signal
that routing is active. The accelerator returns a processed result. Mid price is the only
tracked value at this stage.

### 1.1 Microburst Detector

Extend `TickProcessor` with a dedicated `BurstDetector` component (or inline state).

The detector maintains a short sliding window — a fixed-size array of per-tick inter-arrival
timestamps, populated from `received_at_ns` in `CachedTick`. On every tick it computes the
mean inter-arrival time over the window (e.g. last 16 ticks). If the mean falls below a
configurable threshold `burst_threshold_ns`, the detector enters `BURST` state. It exits
`BURST` state when the mean rises back above a hysteresis threshold
(`burst_exit_threshold_ns > burst_threshold_ns`) to avoid chattering.

State machine:
```
NORMAL  →(mean inter-arrival < burst_threshold_ns)→  BURST
BURST   →(mean inter-arrival > burst_exit_threshold_ns)→  NORMAL
```

The detector exposes a `bool in_burst()` method and a `BurstStats` struct containing:
- current mean inter-arrival time
- burst entry timestamp
- burst tick count so far

Thresholds and window size should be runtime-configurable (read from a config struct at
startup, not hardcoded).

### 1.2 Mid Price Tracking

Mid price is the primary tracked value for the MVP. At this stage the data generator only
produces a single `price` field, so mid price equals that price directly. The `TickProcessor`
computes and maintains:

- **Raw mid price** — the latest observed price
- **EMA mid price** — exponential moving average over the tick stream (alpha configurable)
- **Burst EMA** — a separate, faster EMA (higher alpha) active only during burst state,
  intended to capture rapid directional moves

These values are written to a lightweight `MidPriceState` struct held inside `TickProcessor`.
This struct becomes the primary input to the accelerator batch.

### 1.3 Accelerator Batch Protocol

Expand `SharedMemoryBlock` to include a proper batch transfer region alongside the existing
single-tick `data_to_accelerator` field. The batch region holds:

- A fixed-size array of `AcceleratorTick` entries (e.g. 64 slots)
- An atomic `batch_sequence_number` (producer: runtime, written last with release)
- An atomic `batch_size` field (number of valid entries in the current batch)
- A `BurstMetadata` struct: burst entry time, tick count, EMA snapshot at burst entry

Each `AcceleratorTick` in the array contains:
- sequence number
- raw price
- inter-arrival delta (nanoseconds since previous tick)

The runtime fills the batch incrementally as burst ticks arrive, then commits by writing
`batch_size` (relaxed) and `batch_sequence_number` (release). This is the same
acquire/release handshake already used for `latest_market_data`.

The accelerator's response region (`accelerator_signal`) is extended to include:
- `result_sequence_number` — echoes the batch sequence it is responding to
- `processed_ema` — the accelerator's own EMA computation over the batch
- `signal_action` — integer: `+1` buy bias, `-1` sell bias, `0` neutral
- `routing_active` — atomic bool that the runtime sets to `true` on batch send and the
  accelerator clears when it has consumed the batch

### 1.4 Runtime Routing Logic

In `RuntimeEngine::run()`, after each tick is processed by `TickProcessor`:

```
if detector.in_burst():
    append tick to pending_batch
    if pending_batch is full OR burst just started:
        write batch to shm
        set routing_active = true (release)
        emit routing signal to stdout/log
else:
    if routing_active was true:
        emit "routing ended, burst duration=X ticks" to log
    process tick locally only
```

The routing signal should be a structured log line distinguishable from normal tick output,
e.g. `[ROUTE] seq=N batch_size=M burst_ticks=K ema_at_entry=P.PP`.

The runtime does not wait for the accelerator's response before processing the next tick —
it continues polling. The accelerator response is collected on the next poll iteration where
`accelerator_signal.result_sequence_number` has advanced.

### 1.5 Accelerator Process

`src/accelerator/main.cpp` graduates from stub to real implementation.

The accelerator opens the shm (not creates it — the runtime opens first after data_sim,
and the accelerator opens third), then spins on `batch_sequence_number` with acquire.

On each new batch:
1. Read all `batch_size` ticks (relaxed after acquire on seq)
2. Compute EMA over the batch prices (can use a different alpha than the runtime)
3. Compute a simple directional signal: compare batch EMA to the `BurstMetadata.ema_snapshot`
   — if batch EMA > snapshot by threshold: +1, below by threshold: -1, else 0
4. Write `processed_ema`, `signal_action` (relaxed), then `result_sequence_number` (release)
5. Clear `routing_active` (release)

At this stage the signal logic is intentionally trivial. The accelerator's value is in
demonstrating the offload path and the IPC handshake, not in the quality of the signal.

### 1.6 Shared Memory Layout Changes

The existing `SharedMemoryBlock` needs to grow. Use a versioned layout constant
(`SHM_LAYOUT_VERSION`) so that mismatched binaries fail fast at startup with a clear error
rather than silently reading wrong data. All three processes check this version on attach.

New layout sketch:
```
SharedMemoryBlock
  ├── layout_version           (atomic<uint32_t>)     — written by data_sim at create
  ├── latest_market_data       (MarketData)            — data_sim → runtime  [existing]
  ├── accelerator_batch        (AcceleratorBatch)      — runtime → accelerator [new]
  └── accelerator_signal       (AcceleratorSignal)     — accelerator → runtime [extended]
```

Each region remains alignas(64). The `data_to_accelerator` MarketData field from the
original layout is superseded by `accelerator_batch` and can be removed.

### 1.7 Verification

1. `data_sim` running at normal rate → runtime prints EMA lines, routing_active stays false
2. `data_sim` enters a burst → runtime emits `[ROUTE]` lines, accelerator prints its EMA
   and signal action, routing_active transitions true → false each round trip
3. Burst ends → runtime emits routing-ended log, resumes normal processing
4. Kill accelerator mid-burst → runtime continues (non-blocking), routing_active stays true
   until reset — note this as a known limitation of the MVP, not a bug

---

## Phase 2 — Order Book Data Model

**Goal:** Extend the data generator and shared memory to carry bid/ask depth, not just a
single mid price. This is the prerequisite for any real market-making strategy.

### 2.1 Order Book Data Structures

Define a `PriceLevel` struct: `price` (double) and `quantity` (double or uint64 in ticks).

Define a `OrderBookSnapshot` struct:
- `bid_levels[N]` — top N bid price levels, sorted descending by price
- `ask_levels[N]` — top N ask price levels, sorted ascending by price
- N = 5 for MVP (configurable via a compile-time constant `BOOK_DEPTH`)
- `mid_price` — computed as `(best_bid + best_ask) / 2`
- `spread` — `best_ask - best_bid`
- `sequence_number`, `timestamp` — same handshake as existing `MarketData`

`OrderBookSnapshot` goes into `SharedMemoryBlock` as the replacement for or supplement to
`latest_market_data`. Since all fields must be readable atomically from the consumer side
without a lock, each `price` and `quantity` field is individually atomic, or the entire
snapshot is double-buffered with a sequence-lock pattern (see 2.3).

### 2.2 Order Book Simulator in data_sim

The data generator currently produces a single price via random walk. Extend it with an
`OrderBookGenerator` that:

- Maintains a synthetic mid price via the existing random walk
- Generates a synthetic spread drawn from a log-normal distribution (spread widens during
  bursts, narrows during quiet periods)
- Populates bid and ask levels by placing synthetic depth at fixed tick intervals below/above
  the best quote, with quantities drawn from a configurable distribution (e.g. exponential
  decay away from best)
- During bursts: widens spread, thins near-touch depth (simulates a stressed book)
- Exposes a `next_snapshot()` method analogous to the existing `next()` method

The random walk parameters (volatility, mean reversion speed, burst multipliers) apply to
mid price. Spread and depth have their own parameter sets.

### 2.3 Sequence Lock for the Book Snapshot

A full `OrderBookSnapshot` with 5 levels per side is too large for a single atomic store.
The standard solution is a sequence lock:

- A `seqlock` consists of an atomic counter that the writer increments before and after
  the write (odd = write in progress, even = committed)
- The reader spins until the counter is even, reads all fields with relaxed loads, then
  checks the counter hasn't changed; if it has, retry
- This gives the reader a consistent snapshot without a mutex and without making every
  field atomic

Implement `SeqLock<T>` as a generic template in `src/common/`. `T` must be trivially
copyable. `SharedMemoryBlock` holds a `SeqLock<OrderBookSnapshot>`.

### 2.4 Mid Price Update in Runtime

With the order book snapshot, mid price is computed as `(best_bid.price + best_ask.price) / 2`
rather than reading a single synthetic price. Update `TickProcessor` accordingly.
The burst detector's inter-arrival logic is unchanged — it operates on the sequence number
handshake, not on the price content.

---

## Phase 3 — Market Making Strategy

**Goal:** Implement a basic market-making strategy that uses order book depth to generate
quotes, manage inventory, and compute a spread. The accelerator handles the computationally
intensive parts of the strategy during bursts.

### 3.1 Strategy Overview

A market maker continuously posts both a bid and an ask quote around the current mid price.
Profit comes from the bid-ask spread; risk comes from adverse selection (a directional move
hits one side of the book repeatedly, leaving the market maker with a one-sided inventory).

The core loop:
1. Observe current `OrderBookSnapshot`
2. Compute fair value (a filtered mid price)
3. Compute desired spread (a function of volatility, inventory, and book depth)
4. Compute bid quote = fair value − half_spread, ask quote = fair value + half_spread
5. Adjust quotes for inventory skew: if long, shade both quotes down slightly to attract
   sells; if short, shade up
6. Emit quote decision to a `QuoteOutput` region in shm (consumed by a simulated execution
   layer, not yet implemented in the MVP)

### 3.2 Fair Value Estimation

Fair value is not simply mid price — it needs to filter out noise. Maintain:

- **Micro-price**: a volume-weighted mid that biases toward the side with more depth.
  Formula: `micro_price = (best_bid * ask_qty + best_ask * bid_qty) / (bid_qty + ask_qty)`
  This naturally skews toward the side where liquidity is thin, anticipating price movement.
- **EMA of micro-price**: smoothed fair value with a short half-life (e.g. alpha ≈ 0.1–0.2)
- During bursts: use a faster EMA (higher alpha) to track rapid moves

### 3.3 Spread Model

The quoted spread is a function of:

- **Realized volatility**: estimated from recent tick returns (standard deviation of
  log-return over a rolling window). Wider spread when vol is high.
- **Book imbalance**: ratio `(bid_qty_total - ask_qty_total) / (bid_qty_total + ask_qty_total)`.
  Strong imbalance toward bids suggests upward pressure — widen ask, tighten bid slightly.
- **Inventory penalty**: as inventory deviates from zero, add a penalty term to the spread
  on the side that would increase inventory, and reduce it on the side that would decrease it.
- Minimum spread is a configurable floor (e.g. 1 tick).

### 3.4 Inventory Manager

A simple `InventoryManager` struct tracks:
- `net_position` — running sum of fills (positive = long, negative = short)
- `position_limit` — maximum absolute position before quotes are pulled on one side
- `target_position` — zero for a vanilla market maker
- `skew` — computed as `skew_factor * net_position / position_limit`, added to fair value

If `abs(net_position) >= position_limit`, the engine stops quoting on the side that would
increase the position further (one-sided market).

### 3.5 Accelerator Role in Phase 3

During bursts, the strategy computation (micro-price, spread model, inventory skew) can be
offloaded to the accelerator. The batch sent to the accelerator is extended to include the
full `OrderBookSnapshot` sequence for the burst window, not just raw prices.

The accelerator returns:
- `recommended_fair_value` — the accelerator's EMA computation
- `recommended_spread` — computed from burst-window volatility
- `signal_action` — directional lean: +1 (raise quotes), -1 (lower quotes), 0 (neutral)

The runtime uses the accelerator's output to update its quote parameters when
`routing_active` was true.

### 3.6 Quote Output Region

Add a `QuoteOutput` region to `SharedMemoryBlock`:
- `bid_price`, `bid_quantity` — the engine's desired bid quote (atomics)
- `ask_price`, `ask_quantity` — the engine's desired ask quote (atomics)
- `quote_sequence_number` — incremented each time quotes are updated (release store)
- `one_sided` — bool, true when inventory limit forces a one-sided market

This region is the interface to any downstream execution simulator. In Phase 3 it is only
written, not consumed — the execution layer is a Phase 4+ concern.

### 3.7 Benchmarking Hook

The research question from the README is: at what burst intensity / queue depth does
accelerator offload become net beneficial? Phase 3 should instrument:

- End-to-end latency per tick: `received_at_ns` to quote write timestamp
- Accelerator round-trip latency: batch send timestamp to `result_sequence_number` advance
- CPU-only latency (same strategy computation, no offload) as baseline

Log these to a `LatencyStats` struct that is periodically flushed to a CSV file (not on
every tick — buffer N samples and flush in bulk to avoid I/O on the hot path).

---

## Phase 4 — PCIe Accelerator Emulation via VFIO

**Goal:** Replace the shared-memory accelerator IPC with a proper PCIe device emulation
using `libvfio-user`. This reflects the real architecture where an FPGA or GPU accelerator
is a PCIe endpoint, not a peer process.

### 4.1 VFIO Overview

VFIO (Virtual Function I/O) allows userspace to directly own a PCIe device's BARs (Base
Address Registers) without going through kernel drivers. `libvfio-user` provides a server
library that emulates a PCIe device in userspace — a process can present itself as a PCIe
device to another process that thinks it is talking to real hardware via VFIO ioctls.

In this system:
- `accelerator_sim` becomes a `libvfio-user` server, presenting a synthetic PCIe device
- `runtime_engine` is the VFIO client, using `vfio-ioctl` to map the device's BARs into
  its own address space and DMA-transfer data directly

### 4.2 Device Interface Design

The emulated PCIe device exposes two BARs:

**BAR0 — Control/Status registers (CSR):** A small MMIO region (e.g. 4KB) with:
- `DEVICE_STATUS` register: ready, busy, error
- `BATCH_SIZE` register: how many ticks to process
- `SUBMIT` register: write to trigger processing (doorbell)
- `RESULT_READY` register: set by device when output is valid
- `INTERRUPT_MASK`: for when eventfd interrupts are added

**BAR1 — Data region:** A larger MMIO-mapped region (e.g. 1MB, backed by hugepages) split into:
- Input window: the tick batch, written by the runtime via MMIO writes
- Output window: the processed result, read by the runtime via MMIO reads

The runtime replaces its shm batch-write path with MMIO writes to BAR1, then a doorbell
write to BAR0 `SUBMIT`. It polls `RESULT_READY` (or in a later iteration, waits on an
eventfd interrupt) instead of spinning on an atomic in shared memory.

### 4.3 Hugepages for DMA Buffers

The data transfer buffers (BAR1 data region) should be backed by 2MB hugepages to:
- Reduce TLB pressure on the hot DMA path
- Improve NUMA locality (pin to the NUMA node of the CPU running the runtime)
- Enable `mlock` to prevent page faults on the buffer

Hugepage allocation uses `mmap` with `MAP_HUGETLB | MAP_HUGE_2MB`, or `memfd_create` with
the `MFD_HUGETLB` flag. The hugepage region is registered with the VFIO container's IOMMU
so the emulated device can perform DMA.

### 4.4 Migration Path

To avoid a flag-day rewrite, the batch protocol (what data is sent, what comes back) is
defined as a pure data layout independent of the transport. Phase 1–3 uses the shm path;
Phase 4 replaces only the transport layer. `RuntimeEngine` should have an abstract
`AcceleratorInterface` with two concrete implementations:
- `ShmAccelerator` — the Phase 1–3 shm path
- `VfioAccelerator` — the Phase 4 VFIO/BAR path

Selecting between them is a compile-time or startup-time configuration choice.

### 4.5 Interrupt vs. Poll

The VFIO path initially uses the same polling pattern as the shm path (spin on
`RESULT_READY` in BAR0). A later iteration can switch to eventfd-based interrupts:
the device writes the eventfd when `RESULT_READY` is set, and the runtime blocks on
`epoll_wait` during the inter-burst idle periods only. During bursts, pure polling is
retained to minimize latency.

---

## Cross-Cutting Concerns

### Memory Ordering Policy

All producer/consumer handshakes follow the same pattern throughout the codebase:
- Producer writes payload fields with `memory_order_relaxed`
- Producer writes the sequence number (or version counter) last with `memory_order_release`
- Consumer loads the sequence number with `memory_order_acquire`
- Consumer reads payload fields with `memory_order_relaxed`

This is documented in `src/common/shm_types.hpp` and must be maintained by all new regions.
Any deviation (e.g. a field that needs stronger ordering) must be explicitly justified in a
comment at the point of use.

### Configuration

All thresholds, window sizes, EMA alphas, position limits, and spread floors are grouped
into a single `EngineConfig` struct loaded at startup. No magic numbers in hot-path code.
A future addition is loading config from a file (TOML or a simple key=value format) without
requiring a rebuild.

### Logging

Hot-path logging (per-tick) must be non-blocking. Use a single-producer single-consumer
lock-free queue to pass log records to a dedicated logging thread that does the actual
`write()` syscall. The log record is a fixed-size struct (not a heap-allocated string).
Off the hot path (burst entry/exit, routing events) direct stdout is acceptable.

### Testing

Each component should have a corresponding unit test in `src/tests/`:
- `BurstDetector`: feed synthetic inter-arrival sequences, assert state transitions
- `RingBuffer`: wrap-around, overwrite behaviour
- `TickProcessor`: EMA convergence, tick-rate window
- `SeqLock`: concurrent writer/reader using `std::thread` (not real concurrency, just a
  regression guard)
- `OrderBookGenerator`: statistical properties of generated spreads/depths
- `InventoryManager`: skew and one-sided-market transitions

Integration test: launch data_sim and runtime_engine as child processes (via `fork`/`exec`),
let them run for N ticks, assert that the runtime's output log contains the expected
routing events.

---

## File Map by Phase

```
Phase 1
  src/common/shm_types.hpp          — extend SharedMemoryBlock, add AcceleratorBatch
  src/common/shm_manager.hpp/.cpp   — no change needed
  src/data/main.cpp                 — no change needed (burst already implemented)
  src/runtime/runtime_engine.hpp    — add BurstDetector, routing state, batch accumulator
  src/runtime/runtime_engine.cpp    — implement routing logic
  src/accelerator/accelerator.hpp   — new: AcceleratorEngine class
  src/accelerator/accelerator.cpp   — new: batch consumer + signal computation
  src/accelerator/main.cpp          — wire up AcceleratorEngine

Phase 2
  src/common/shm_types.hpp          — add OrderBookSnapshot, SeqLock<T>
  src/common/seqlock.hpp            — new: SeqLock<T> template
  src/data/order_book_generator.hpp — new: OrderBookGenerator
  src/data/order_book_generator.cpp — new
  src/data/main.cpp                 — use OrderBookGenerator, write snapshots to shm
  src/runtime/runtime_engine.cpp    — read OrderBookSnapshot, update mid price

Phase 3
  src/runtime/strategy.hpp          — new: FairValueEstimator, SpreadModel, InventoryManager
  src/runtime/strategy.cpp          — new
  src/common/shm_types.hpp          — add QuoteOutput region
  src/runtime/runtime_engine.cpp    — integrate strategy, write QuoteOutput

Phase 4
  src/accelerator/vfio_device.hpp   — new: VfioAccelerator (libvfio-user server)
  src/accelerator/vfio_device.cpp   — new
  src/runtime/accelerator_iface.hpp — new: abstract AcceleratorInterface
  src/runtime/shm_accelerator.hpp   — new: ShmAccelerator (wraps Phase 1–3 path)
  src/runtime/vfio_accelerator.hpp  — new: VfioAccelerator client
  CMakeLists.txt                    — add libvfio-user linkage
```
