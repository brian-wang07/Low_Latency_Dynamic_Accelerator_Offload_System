# low latency engine
<p align="center">
  <img src="./assets/dashboard-animation.gif"/>
</p>
A userspace market-making engine with synthetic order flow, microburst detection, and hardware accelerator offload. The system streams a realistic order book over shared memory at low latency, detects microbursts in real time, and offloads computationally intensive strategy work to a separate accelerator — eventually a physical FPGA via VFIO/PCIe.

---

## architecture

```
┌────────────────────────────────────────────────┐                                                                   
│              EXCHANGE (Process 1)              │                                                                   
│                                                │                                                                   
│     [CREATOR]───────HEAP────────►[DISPATCHER]──┼─────────┐ 
│                                                │         │   
│   [ADVERSARIES]────────────────────────────────┼──────┐  │  give each producer its own spsc ring,                       
│                                                │     ┌▼──▼┐ and k-way merge on the enqueue timestamp.
│ [MATCHING ENGINE]◄─────────────────────────────┼─────┤SPSC◄─────┐                                                  
│         │                                      │     └────┘     │                                                  
└─────────┼──────────────────────────────────────┘    EXCHANGE    │                                                  
       ┌──▼─┐                                                     │                                                  
       │SPSC│ENGINE                                               │                                                  
       └──┬─┘                                                     │                                                  
┌─────────┼──────────────────────────────────────┐                │                                                  
│         │ RUNTIME ENGINE (Process 2)           │                │                                                  
│         │                                      │ DASHBOARD      │                                                  
│         ▼         ┌───────┐                    │ ┌────┐         │                                                  
│    [HOT PATH]─────►Seqlock┼────►[SNAPSHOTTER]──┼─►SPSC│         │                                                  
│         │         └───────┘                    │ └──┬─┘         │                                                  
└─────────┼──────────────────────────────────────┘    │           │                                                  
          │                                           │           │                                                  
       ┌──▼─┐                      ┌──────────────────┴──┐        │                                                  
       │SPSC│STRATEGY              │DASHBOARD (Process 4)│        │                                                  
       └──┬─┘                      └─────────────────────┘        │                                                  
          │                                                       │                                                  
┌─────────┼──────────────────────────────────────┐                │                                                  
│         │    STRATEGY (Process 3)              │                │                                                  
│         ▼   Dispatch to main or offload        │                │                                                  
│   [DISPATCHER]───────────────────────►[MAIN]   │                │                                                  
│         │                                │     │                │                                                  
│         ▼                                │     │                │                                                  
│   [ACCELERATOR]──────────────────────────┴─────┼────────────────┘                                                  
│                                                │                                                                   
└────────────────────────────────────────────────┘                                                                   
```

---

## shared memory layout

```
SharedMemoryBlock  (/engine_shm_mvp, 16MB, versioned)
  ├── ShmHeader              magic + version check (fail-fast on mismatch)
  ├── ExchangeInputArray     producers → matching engine   2 × SPSC, 4096 × 64-byte slots
  ├── EventRingBuffer        matching engine → runtime     SPSC, 8192 × 64-byte slots
  ├── StrategyRingBuffer     runtime → strategy            SPSC, 4096 × StrategyTick slots
  ├── BookSnapshot           runtime → dashboard           seqlock-protected, written at 30 Hz
  ├── AcceleratorBatch       runtime → accelerator         64-tick burst batch + metadata
  └── AcceleratorSignal      accelerator → runtime         EMA result, signal action, routing flag
```

All producer/consumer handshakes use the same acquire/release pattern: payload written relaxed, sequence number written last with release; consumer acquires on sequence, reads payload relaxed.

---

## data generator

Synthetic order flow parameterized per instrument via JSON configs in `configs/`. Key processes:

| process | description |
|---|---|
| RegimeMachine | 3-state Markov chain (CALM/ACTIVE/BURST), single uniform draw per event |
| OU mid-price | GBM for crypto spot/perp, logit-space random walk for prediction markets |
| OU spread | Ornstein-Uhlenbeck, hard floor at 1 tick |
| Limit placement | Truncated geometric distribution from BBO |
| Marketable limits | Uniform draw through opposing best price, executes then rests |
| Price impact | Fill size / BBO depth nudges mid-price |
| SidePool tracking | Fixed-size (512/side), zero heap alloc, eviction emits CANCEL |
| Crossed/stale sweep | On BBO move, cancel crossed and far-from-BBO orders (budget: 32) |

Available configs: `btc_spot`, `btc_perp`, `eth_perp`, `polymarket_btc_100k`.

---

## strategy interface

The dispatcher thread provides a clean interface for implementing trading strategies:

```cpp
// Called on every tick in normal mode
struct BaseStrategy {
    virtual void on_tick(StrategyContext& ctx) noexcept = 0;
};

// Called during detected microbursts
struct AcceleratorStrategy {
    virtual void on_burst_tick(StrategyContext& ctx) noexcept = 0;
    virtual void on_burst_end(StrategyContext& ctx) noexcept = 0;
};
```

`StrategyContext` provides read-only market state (BBO, spread, EMA, burst stats) and a write interface (`place_limit`, `cancel`) that queues orders back to the orchestrator's book via the dispatcher→orchestrator ring.

---

## processes

| binary | role |
|---|---|
| `data_sim` | generates synthetic order events, writes to ticks ring buffer |
| `runtime_engine` | orchestrator (book reconstruction, snapshots) + dispatcher (strategy execution) |
| `accelerator_sim` | consumes burst batches, computes signal, returns result |
| `dashboard` | standalone GUI process, reads snapshots from dashboard shared memory |
| `data_test` | standalone calibration run — prints event stream + stats summary |
| `order_book_test` | unit tests for the order book |

---

## roadmap

### complete
- Stochastic data generator (RegimeMachine + OU) with per-asset-class dynamics
- SPSC ring buffer over POSIX shared memory
- Runtime order book (bid/ask level maps, tracked orders)
- Microburst detector (sliding window inter-arrival mean, hysteresis)
- Accelerator batch offload over shared memory (EMA + directional signal)
- Marketable limit orders with immediate execution and passive residual resting
- Dear ImGui + ImPlot dashboard (order book, price plots, latency, imbalance)

### in progress
- Fix order book corruption (level erasure bug in on_execute, ADD_LIMIT_MKT drop)
- Decouple GUI into standalone dashboard process with dedicated shared memory
- Split runtime engine into orchestrator + dispatcher threads (CPU-pinned)
- BaseStrategy / AcceleratorStrategy interfaces with StrategyContext
- Dispatcher→orchestrator order writeback for strategy impact on book

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
Before running, ensure that hugepages is set up (native Linux only — WSL2 will fall back to POSIX shm with a warning):

```sh
# allocate hugepages: SHM_SIZE / hugepage size = 16MB / 2MB = 8 pages minimum
# allocate a few extra to leave headroom for the OS
echo 10 | sudo tee /proc/sys/vm/nr_hugepages

# mount hugetlbfs so the engine can open a named hugepage-backed file
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs -o pagesize=2M hugetlbfs /dev/hugepages
```

If you change `SHM_SIZE` in `shm_core.hpp`, update `nr_hugepages` accordingly: `ceil(SHM_SIZE / 2MB)`, plus a few spare.

Start processes in order (each opens shm after the previous creates it):

```sh
./build/data_sim configs/btc_perp.json &
./build/runtime_engine &
./build/accelerator_sim &
./build/dashboard &
```

Or run the standalone calibration test without shared memory:

```sh
./build/data_test
```
