#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace engine::shm {

inline constexpr const char*   SHM_NAME = "/engine_shm_mvp";
inline constexpr std::size_t   SHM_SIZE = 1024 * 1024; // 1MB for MVP
inline constexpr std::uint32_t SHM_MAGIC = 0x454E474E; //version validation
inline constexpr std::uint32_t SHM_VERSION = 1;

struct alignas(64) ShmHeader {
    uint32_t magic;
    uint32_t version;
};

struct alignas(64) MarketData {
    std::atomic<uint64_t> sequence_number;
    std::atomic<double>   price;
    std::atomic<uint64_t> timestamp;
};

struct alignas(64) AcceleratorSignal {
    std::atomic<uint64_t> sequence_number;
    std::atomic<int>      action; // e.g., 1 for buy, -1 for sell, 0 for hold
};

struct SharedMemoryBlock {
    alignas(64) ShmHeader         header;
    alignas(64) MarketData        latest_market_data; //data -> runtime
    alignas(64) MarketData        data_to_accelerator; //runtime -> accelerator
    alignas(64) AcceleratorSignal accelerator_signal; //accelerator -> runtime
};

static_assert(offsetof(SharedMemoryBlock, latest_market_data) % 64 == 0);
static_assert(offsetof(SharedMemoryBlock, data_to_accelerator) % 64 == 0);
static_assert(offsetof(SharedMemoryBlock, accelerator_signal) % 64 == 0);
static_assert(sizeof(SharedMemoryBlock) <= SHM_SIZE);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

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

