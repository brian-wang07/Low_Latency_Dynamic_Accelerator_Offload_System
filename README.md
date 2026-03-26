# low-latency engine

A userspace market-making engine with synthetic order flow, microburst detection, and hardware accelerator offload. The system streams a realistic order book over shared memory at low latency, detects microbursts in real time, and offloads computationally intensive strategy work to a separate accelerator — eventually a physical FPGA via VFIO/PCIe.

---

## architecture

```
  ┌─────────────────────────────────────────────────────────────────────────┐
  │  data_sim  (process)                                                    │
  │                                                                         │
  │  DataGenerator                                                          │
  │  ├── OU mid-price + CIR spread (stochastic processes)                   │
  │  ├── Hawkes process (self-exciting event arrivals)                      │
  │  ├── Truncated geometric limit order placement                          │
  │  ├── Marketable limit orders (ADD_LIMIT_MKT)                            │
  │  ├── Market orders w/ price impact                                      │
  │  └── Burst mode (volatility + rate multiplier)                          │
  │                          │                                              │
  │                          │  EventRingBuffer (SPSC, 8192 slots)          │
  │                          │  ShmOrderEvent: ADD_LIMIT / ADD_LIMIT_MKT /  │
  │                          │  ADD_MARKET / CANCEL / EXECUTE               │
  └──────────────────────────┼──────────────────────────────────────────────┘
                             │ POSIX shared memory  (/engine_shm_mvp)
  ┌──────────────────────────▼──────────────────────────────────────────────┐
  │  runtime_engine  (process)                                              │
  │                                                                         │
  │  engine thread  ──── spin-polls ring buffer (_mm_pause)                 │
  │  ├── OrderBook  (bid/ask price-level maps, tracked orders)              │
  │  ├── BurstDetector  (16-event sliding window, configurable thresholds)  │
  │  ├── TickProcessor  (EMA + tick-rate window)                            │
  │  └── on burst: fills AcceleratorBatch → writes to shm                  │
  │                          │                                              │
  │  book snapshot  ◄────────┘  (seqlock, written every N events)          │
  │        │                                                                │
  │  GUI thread  (60 hz render loop)                ◄── future              │
  │  ├── Dear ImGui + ImPlot                                                │
  │  ├── market book panel  (live bid/ask depth)                            │
  │  ├── price trend panel  (mid, EMA, spread)                              │
  │  ├── MM strategy loader  (select + configure strategies)                │
  │  └── accelerator status  (routing active, signal action, latency)       │
  └───────────┬─────────────────────────────────────────────────────────────┘
              │ AcceleratorBatch  (runtime → accelerator)
              │ AcceleratorSignal (accelerator → runtime)
              │
  ┌───────────▼─────────────────────────────────────────────────────────────┐
  │  accelerator_sim  (process)                                             │
  │                                                                         │
  │  current: shared memory IPC                                             │
  │  ├── spins on batch_sequence_number                                     │
  │  ├── computes EMA over burst batch                                      │
  │  ├── emits signal_action (+1 / 0 / -1) + processed_ema                 │
  │  └── clears routing_active on completion                                │
  │                                                                         │
  │  future: VFIO/PCIe (external FPGA)                                      │
  │  ├── libvfio-user server presenting synthetic PCIe device               │
  │  ├── BAR0: control/status registers (SUBMIT doorbell, RESULT_READY)     │
  │  ├── BAR1: hugepage-backed data region (tick batch in, result out)      │
  │  └── physical target: De1-SoC or similar FPGA over PCIe                │
  └─────────────────────────────────────────────────────────────────────────┘
```

---

## shared memory layout

```
SharedMemoryBlock  (/engine_shm_mvp, 1MB, versioned)
  ├── ShmHeader              magic + version check (fail-fast on mismatch)
  ├── EventRingBuffer        data_sim → runtime      SPSC, 8192 × 64-byte slots
  ├── AcceleratorBatch       runtime → accelerator   64-tick burst batch + metadata
  └── AcceleratorSignal      accelerator → runtime   EMA result, signal action, routing flag
```

All producer/consumer handshakes use the same acquire/release pattern: payload written relaxed, sequence number written last with release; consumer acquires on sequence, reads payload relaxed.

---

## data generator

Synthetic order flow parameterized per instrument via JSON configs in `configs/`. Key processes:

| process | description |
|---|---|
| OU mid-price | Ornstein-Uhlenbeck mean reversion |
| CIR spread | Cox-Ingersoll-Ross, hard floor at 1 tick |
| Hawkes arrivals | Self-exciting process, separate bid/ask intensities |
| Limit placement | Truncated geometric distribution from BBO |
| Marketable limits | Uniform draw through opposing best price, executes then rests |
| Price impact | Fill size / BBO depth nudges mid-price |
| Burst mode | Configurable volatility + rate multipliers, Bernoulli onset |

Available configs: `aapl`, `btc_perp_futures`, `eth_perp_futures`, `sp500_futures`, `eur_spotfx_futures`, `10y_treasury_futures`.

---

## processes

| binary | role |
|---|---|
| `data_sim` | generates synthetic order events, writes to ring buffer |
| `runtime_engine` | consumes ring buffer, maintains order book, detects bursts, routes to accelerator |
| `accelerator_sim` | consumes burst batches, computes signal, returns result |
| `data_test` | standalone calibration run — prints event stream + stats summary |
| `order_book_test` | unit tests for the order book |

---

## roadmap

### complete
- Stochastic data generator (OU + CIR + Hawkes) with burst mode
- SPSC ring buffer over POSIX shared memory
- Runtime order book (bid/ask level maps, tracked orders)
- Microburst detector (sliding window inter-arrival mean, hysteresis)
- Accelerator batch offload over shared memory (EMA + directional signal)
- Marketable limit orders with immediate execution and passive residual resting

### next: dashboard (Dear ImGui)
- GUI thread in the runtime_engine process reads a seqlock book snapshot at 60 hz
- Market book panel: live bid/ask depth up to N levels
- Price trend panel: mid price, EMA, spread over time (ImPlot scrolling buffer)
- Instrument config picker: load any `configs/*.json` at runtime
- MM strategy panel: select and configure strategy parameters
- Accelerator status panel: routing active, signal action, round-trip latency histogram

### future: market-making strategy
- Micro-price fair value (volume-weighted mid)
- Spread model: realized volatility + book imbalance + inventory penalty
- Inventory manager with position limits and quote skew
- Quote output region in shared memory for downstream execution layer
- Accelerator offloads full strategy computation during bursts

### future: VFIO/PCIe accelerator
- `accelerator_sim` becomes a `libvfio-user` server presenting a synthetic PCIe endpoint
- BAR0: MMIO control/status registers (SUBMIT doorbell, RESULT_READY poll)
- BAR1: hugepage-backed DMA buffer (2MB pages, IOMMU-registered)
- Runtime replaces shm batch-write with MMIO writes + doorbell; polls BAR0 for result
- Abstract `AcceleratorInterface` with `ShmAccelerator` and `VfioAccelerator` implementations for zero-flag-day migration
- Physical target: De1-SoC FPGA connected over PCIe
- Research question: at what burst intensity / queue depth does PCIe offload latency amortize and win over CPU-local execution?

---

## build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Requires C++23, CMake 3.20+, Linux (POSIX shm, `_mm_pause`).

---

## running

Start processes in order (each opens shm after the previous creates it):

```sh
./build/runtime_engine &
./build/accelerator_sim &
./build/data_sim configs/btc_perp_futures.json
```

Or run the standalone calibration test without shared memory:

```sh
./build/data_test
```
