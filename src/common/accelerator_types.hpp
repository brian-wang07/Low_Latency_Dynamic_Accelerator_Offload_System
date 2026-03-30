#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace accelerator {
inline constexpr std::uint8_t  BATCH_SIZE = 64;
struct AcceleratorTick {
    // we dont need atomics here because all acceleratorticks within a burst will be sent in a batch atomically;
    // ticks themselves can be nonatomic
    //one consumer one producer => dont need padding
    uint64_t sequence_number;
    int64_t  price;             // fixed-point (× PRICE_SCALE)
    uint64_t arrival_delta_ns; //time between ticks sent to accelerator
};

struct alignas(64) BurstMetadata {
    // runtime state at the moment burst started
    uint64_t burst_entry_time_ns;
    uint64_t tick_count;
    int64_t  ema_at_entry;      // fixed-point (× PRICE_SCALE)
};

struct alignas(64) AcceleratorBatch {
    std::atomic<uint64_t> batch_sequence_number;    
    uint32_t              count;
    //64 - 8 - 4 = 52    
    uint8_t               _pad[52];

    // cache line 2:
    BurstMetadata         burst_meta;

    // cache line 3+:
    AcceleratorTick       ticks[BATCH_SIZE];
};

struct alignas(64) AcceleratorSignal {
    std::atomic<uint64_t> result_sequence_number;
    std::atomic<int64_t>  processed_ema;    // fixed-point (× PRICE_SCALE)
    std::atomic<int8_t>   signal_action;
    std::atomic<bool>     routing_active;
};

}
