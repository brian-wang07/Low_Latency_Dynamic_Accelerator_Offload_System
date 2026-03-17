## Plan: Data Simulator Implementation

The goal is to implement the market data simulator process (`data_sim`), which manages the lifecycle of the shared memory mapping, generates prices using a random walk, and generates market microbursts simulating real high-frequency environments.

**Steps**
1. **Shared Memory Mapping**
   - Create a robust `ShmManager` RAII class wrapping `shm_open`, `ftruncate`, and `mmap` with `PROT_READ | PROT_WRITE`.
   - Setup a signal handler (SIGINT/SIGTERM) to cleanly call `shm_unlink`.
2. **Random Walk Core**
   - Implement a simple normal distribution random walk: $P_t = P_{t-1} + dt \cdot \mu + \sigma \cdot Z_t \cdot \sqrt{dt}$, keeping prices realistic (positive).
   - Use `std::mt19937` or standard fast RNG implementations with distributions from `<random>`.
3. **Microburst Generation**
   - Implement a burst probability variable ($P_{burst}$).
   - Run a loop that typically sleeps for an interval (e.g., 500µs - 5ms).
   - When a burst triggers: output $N$ sequential price updates back-to-back with minimal or no sleeping.
4. **Zero-Copy Lock-Free Updating**
   - Publish to `latest_market_data` in `SharedMemoryBlock`.
   - **Crucial Ordering:** Write `price` and `timestamp` using `std::memory_order_relaxed`, followed by `sequence_number.store(seq, std::memory_order_release)`. The runtime spins relying on `sequence_number.load(std::memory_order_acquire)`.
5. **Timestamping Strategy**
   - Utilize standard POSIX `clock_gettime(CLOCK_MONOTONIC, ...)` or x86 `__rdtsc()` intrinsic for ultra-low latency timing if required by the pipeline. For now, `std::chrono::high_resolution_clock` or `clock_gettime` should suffice for the MVP.

**Relevant files**
- `src/common/shm_types.hpp` — Defines the target memory structure (already aligned to cachelines). Let's review if constructors/init functions are needed.
- `src/data/main.cpp` — Where the `ShmManager` and loops will be implemented.
- `src/data/shm_manager.hpp` / `.cpp` (To be created) — Encapsulated SHM initialization logic.

**Verification**
1. Run `data_sim` and manually inspect `/dev/shm/engine_shm_mvp` to confirm file is written and sized to 1MB.
2. Build a minimal debug logger within `data_sim` (outputting every 10,000th sequence) to confirm throughput and sequence number increments.

**Decisions**
- The Data process will acts as the Owner/Host of the SHM region, responsible for creating and linking/unlinking it.
- Updates utilize standard atomics with release/acquire semantics rather than heavy OS synchronization primitives.

**Further Considerations**
1. Do you want to explicitly pin this simulator to a specific CPU core (`sched_setaffinity`) to prevent context switching, or leave CPU configuration default for the MVP?
2. How heavy should the microbursts be? (e.g. baseline 1k MSGs/s, bursting up to 100k MSGs/s for brief fractions of a second?)
