#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace engine::shm {

inline constexpr const char* SHM_NAME = "/engine_shm_mvp";
inline constexpr std::size_t SHM_SIZE = 1024 * 1024; // 1MB for MVP

// Example shared structure using atomics for zero-copy, lock-free communication
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

} // namespace engine::shm
