# Strategy-Driven Accelerator Routing

## Context

Currently `RuntimeEngine` routes to the accelerator based on **external timing**: the `BurstDetector`
measures mean inter-arrival time of ticks, and routes when ticks arrive faster than a threshold.

The proposed change routes based on **internal computational load**: the runtime continuously evaluates
a strategy, and routes to the accelerator when that strategy evaluation can no longer keep up with
the tick arrival rate. This is a semantically different and more meaningful trigger — it measures
actual backlog rather than inferring it from tick rate.

---

## What Changes vs. What Stays the Same

### Unchanged (no modifications needed)
- `shm_types.hpp` — batch protocol, `AcceleratorBatch`, `AcceleratorSignal`, handshake ordering
- `shm_manager.cpp` — POSIX shm lifecycle unchanged
- Accelerator process — batch consumer + signal computation unchanged
- `data_sim` — unchanged
- `RingBuffer`, `TickProcessor` — unchanged

### Changed: Routing Trigger (small)

Replace (or supplement) `BurstDetector` with a **ProcessingLagDetector** that tracks:

```
lag = tick_received_at_ns - strategy_eval_completed_at_ns
```

If `lag > threshold` → route. This means the strategy fell behind: a new tick arrived
before the previous strategy evaluation finished.

The class structure is nearly identical to `BurstDetector` — same hysteresis pattern,
same `in_burst()` / `stats()` interface. The difference is the input: instead of
inter-arrival deltas, it receives `(tick_received_at_ns, eval_completed_at_ns)` pairs.

Changes in `src/runtime/runtime_engine.hpp`: rename/replace `BurstDetector` with `ProcessingLagDetector`.
Changes in `src/runtime/runtime_engine.cpp`: call `detector_.on_tick(received_at, eval_end_ns)`
instead of `detector_.on_tick(received_at)`.

### Changed: Strategy Computation (medium — Phase 3 prerequisite)

Without actual strategy work in the hot loop, `eval_end_ns ≈ received_at_ns` always
(EMA is nanoseconds). The routing trigger only becomes meaningful once the runtime is doing
real strategy evaluation (Phase 3: micro-price, spread model, inventory skew).

This is the bulk of the work. The strategy classes (`FairValueEstimator`, `SpreadModel`,
`InventoryManager`) live in `src/runtime/strategy.hpp/.cpp` per plan.md §3.

### Not changed: Batch accumulation logic

The logic that fills `pending_batch_[]` and commits to shm remains the same. The only
difference is *what* decides to trigger it.

---

## Scope Summary

| Component | Size of change |
|---|---|
| Routing trigger (`BurstDetector` → `ProcessingLagDetector`) | Small (~50 lines) |
| Strategy implementation (Phase 3 prerequisite) | Medium-large (new files) |
| Everything else | Zero |

**The shm protocol, accelerator, and batch logic are not touched at all.**

The architectural shape stays identical — only the signal that says "go offload now"
changes from "ticks are arriving fast" to "I'm falling behind processing them."

---

## Recommended Sequencing

1. Finish Phase 1.5 (accelerator stub → real implementation) — validates the IPC round-trip
2. Implement Phase 2 (order book data model) — gives the strategy real inputs
3. Implement Phase 3 strategy in runtime — makes lag detection meaningful
4. Replace `BurstDetector` with `ProcessingLagDetector` — now the routing trigger is real

Alternatively: implement `ProcessingLagDetector` now as a drop-in alongside `BurstDetector`
(it will never fire with the current trivial EMA), and activate it once the strategy is in place.

---

## Files to Modify When Implementing

- `src/runtime/runtime_engine.hpp` — add/replace detector class
- `src/runtime/runtime_engine.cpp` — change routing trigger call site
- `src/runtime/strategy.hpp` — new (Phase 3 prerequisite)
- `src/runtime/strategy.cpp` — new (Phase 3 prerequisite)
