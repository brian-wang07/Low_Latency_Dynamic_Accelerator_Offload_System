# Runtime Engine Implementation Plan

## Existing Layout (from shm_types.hpp)
```
SharedMemoryBlock
  ├── latest_market_data    (MarketData)   — data generator → runtime
  ├── data_to_accelerator   (MarketData)   — runtime → accelerator
  └── accelerator_signal    (AcceleratorSignal) — accelerator → runtime
```
Each region is alignas(64) (cache-line isolated). All fields are `std::atomic`.

## Pre-Implementation Fixes Required

### Write ordering in data generator (`src/data/main.cpp`)
Currently `sequence_number` is stored *first*, then `price`, then `timestamp`.
This means the runtime can observe a new sequence number before price/timestamp
are committed. Fix: store price and timestamp first, sequence_number last with
`memory_order_release`. The runtime reads sequence_number with
`memory_order_acquire` — this establishes the happens-before relationship that
makes price/timestamp safe to read without further synchronization.

### SHM name mismatch
`ShmManager` hardcodes `name_ = "/market_data_shm"`, but `shm_types.hpp`
defines `SHM_NAME = "/engine_shm_mvp"`. Both processes must agree on the name.
Either make `ShmManager` accept a name parameter, or use `SHM_NAME` as the
default. Fix before the runtime can attach.

## Class Structure

```
RuntimeEngine
  ├── ShmManager shm_                    — opened (not created)
  ├── RingBuffer<CachedTick, 1024> cache_
  ├── TickProcessor processor_
  └── run()                              — polling loop
```

### `CachedTick` (local, non-atomic)
```cpp
struct CachedTick {
    uint64_t sequence_number;
    double   price;
    uint64_t data_timestamp_ns;   // from MarketData::timestamp
    uint64_t received_at_ns;      // steady_clock::now() at observation
};
```

### `RingBuffer<T, N>`
Fixed-size circular buffer, power-of-2 capacity.
Write head advances on each new tick; oldest entries are silently overwritten.
Single-threaded (runtime owns it exclusively).

### `TickProcessor`
Called on each new tick. Maintains:
- **EMA price** — `ema = alpha * price + (1 - alpha) * ema` (alpha configurable)
- **Tick arrival rate** — count ticks per second over a sliding window
- Prints a status line every N ticks (not every tick — avoid I/O bottleneck)

## Polling Loop

```
open shm (retry until data_sim has created it)
last_seen_seq = 0

loop:
  seq = block->latest_market_data.sequence_number.load(acquire)
  if seq == last_seen_seq:
    _mm_pause() or yield, continue
  else:
    price = block->latest_market_data.price.load(relaxed)
    ts    = block->latest_market_data.timestamp.load(relaxed)
    received_at = steady_clock::now()
    cache_.push({seq, price, ts, received_at})
    processor_.on_tick(...)
    last_seen_seq = seq
```

Relaxed loads for price/timestamp are safe *only after* the write ordering fix
(seq_number written last with release). The acquire on seq_number synchronizes
the entire prior store sequence.

**Spin policy:** Pure spin with `_mm_pause()` (x86 PAUSE instruction) in the
inner loop to reduce pipeline pressure without yielding the thread. No sleep.

## Future Accelerator Interface (not implemented now)
The `data_to_accelerator` and `accelerator_signal` fields in `SharedMemoryBlock`
are already reserved. When implemented:
- Runtime batches N ticks and writes to `data_to_accelerator`, then bumps its
  sequence_number (release)
- Accelerator polls that sequence_number, processes, writes `accelerator_signal`
- Runtime polls `accelerator_signal.sequence_number` for the response

Same producer/consumer handshake as data→runtime, just in the other direction.

## Files Modified for Pre-Implementation Fixes
- `src/data/main.cpp` — reorder the three `.store()` calls (seq_number last)
- `src/common/shm_manager.hpp` — default name uses `engine::shm::SHM_NAME`

## Files Created During Implementation
- `src/runtime/runtime_engine.hpp` — `RuntimeEngine`, `RingBuffer`, `TickProcessor`
- `src/runtime/runtime_engine.cpp` — implementations
- `src/runtime/main.cpp` — construct `RuntimeEngine`, call `run()`
- `CMakeLists.txt` — add `runtime_engine.cpp` to `runtime_engine` target sources

## Verification
1. Start `data_sim` in one terminal
2. Start `runtime_engine` in another — should open shm and begin printing EMA
   and tick-rate lines
3. Kill `data_sim` — runtime should stall (no new ticks) but not crash
4. Restart `data_sim` — runtime resumes from next sequence number
