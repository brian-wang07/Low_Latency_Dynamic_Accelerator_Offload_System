#pragma once
 
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace strategy::shm {
// Buffer 3: strategy dispatch. condensed orderbook data is sent to be dispatched. owned by strategy
inline constexpr std::uint32_t STRATEGY_RING_CAPACITY = 4096;
inline constexpr std::uint32_t STRATEGY_RING_MASK     = STRATEGY_RING_CAPACITY - 1;
struct alignas(64) StrategyTick {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t enqueue_tsc;
    int64_t best_bid;
    int64_t best_ask;
    double spread;
    double vwmid;
    int64_t ema;
};

struct alignas(64) StrategyRingBuffer {
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) StrategyTick slots[STRATEGY_RING_CAPACITY];
};
}
