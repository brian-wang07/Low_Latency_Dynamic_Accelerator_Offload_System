#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace engine::shm {

inline constexpr const char*   SHM_NAME = "/engine_shm_mvp";
inline constexpr std::size_t   SHM_SIZE = 1024 * 1024; // 1MB for MVP
inline constexpr std::uint32_t SHM_MAGIC = 0x454E474E; //version validation
inline constexpr std::uint32_t SHM_VERSION = 1;

inline constexpr std::uint8_t BATCH_SIZE = 64;

struct AcceleratorTick {
    // we dont need atomics here because all acceleratorticks within a burst will be sent in a batch atomically; 
    // ticks themselves can be nonatomic
    //one consumer one producer => dont need padding
    uint64_t sequence_number;
    double   price;
    uint64_t arrival_delta_ns; //time between ticks sent to accelerator
};

struct alignas(64) BurstMetadata {
    // runtime state at the moment burst started
    uint64_t burst_entry_time_ns;
    uint64_t tick_count;
    double   ema_at_entry;
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
    std::atomic<double>   processed_ema;
    std::atomic<int8_t>   signal_action;
    std::atomic<bool>     routing_active;
};


struct alignas(64) ShmHeader {
    uint32_t magic;
    uint32_t version;
};

struct alignas(64) MarketData {
    std::atomic<uint64_t> sequence_number;
    std::atomic<double>   bid;
    std::atomic<double>   ask;
    std::atomic<uint32_t> depth;
    std::atomic<double>   price; //mid = (bid + ask) / 2
    std::atomic<uint8_t>  order_type;
    std::atomic<uint8_t>  side;
    std::atomic<uint8_t>  action;
    uint8_t               _pad[5];
    std::atomic<uint64_t> timestamp;
};


struct SharedMemoryBlock {
    alignas(64) ShmHeader         header;
    alignas(64) MarketData        latest_market_data; //data -> runtime
    alignas(64) AcceleratorBatch  data_to_accelerator; //runtime -> accelerator
    alignas(64) AcceleratorSignal accelerator_signal; //accelerator -> runtime
};

static_assert(offsetof(SharedMemoryBlock, latest_market_data) % 64 == 0);
static_assert(offsetof(SharedMemoryBlock, data_to_accelerator) % 64 == 0);
static_assert(offsetof(SharedMemoryBlock, accelerator_signal) % 64 == 0);
static_assert(sizeof(SharedMemoryBlock) <= SHM_SIZE);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int8_t>::is_always_lock_free);
static_assert(std::atomic<uint8_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

inline void shm_init_header(SharedMemoryBlock *block) noexcept {
    block->header.magic   = SHM_MAGIC;
    block->header.version = SHM_VERSION;
}

[[nodiscard]] 
inline bool shm_validate_header(const SharedMemoryBlock *block) noexcept {
    return block->header.magic   == SHM_MAGIC
        && block->header.version == SHM_VERSION;
}

} // namespace engine::shm

