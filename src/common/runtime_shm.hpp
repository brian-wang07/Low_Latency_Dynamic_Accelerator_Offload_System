#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "shm_core.hpp"

namespace runtime::shm {

// Buffer 2: streamed event data. matching has been done; consumer passively reconstructs orderbook. owned by runtime engine
// PRODUCER: matching engine
// CONSUMER: runtime engine hot thread
inline constexpr std::uint32_t EVENT_RING_CAPACITY = 8192;
inline constexpr std::uint32_t EVENT_RING_MASK     = EVENT_RING_CAPACITY - 1;
struct alignas(64) EventRingBuffer {
    alignas(64) std::atomic<uint64_t> head{0};  // next slot producer will write (producer-owned)
    alignas(64) std::atomic<uint64_t> tail{0};  // next slot consumer will read (consumer-owned)
    alignas(64) common::shm::ShmOrderEvent slots[EVENT_RING_CAPACITY];
};


}
