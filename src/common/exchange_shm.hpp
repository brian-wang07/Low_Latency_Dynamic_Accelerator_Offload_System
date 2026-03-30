#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "shm_core.hpp"

namespace exchange::shm {

// Buffer 1: matching engine feed. an array of spsc buffers. establishes price-time priority
// PRODUCERS: data dispatcher, strategy, and adversaries 
// CONSUMER: matching engine
inline constexpr std::size_t   MAX_EXCHANGE_PRODUCERS = 2; // dispatcher + strategy; add 1 for every adversary
inline constexpr std::uint32_t EXCHANGE_RING_CAPACITY = 4096;                                                    
inline constexpr std::uint32_t EXCHANGE_RING_MASK     = EXCHANGE_RING_CAPACITY - 1;

struct alignas(64) ExchangeRingBuffer {
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) common::shm::ShmOrderEvent slots[EXCHANGE_RING_CAPACITY];
};

struct alignas(64) ExchangeInputArray {
    // Array of SPSC buffers. owned by exchange.
    ExchangeRingBuffer ExchangeRings[MAX_EXCHANGE_PRODUCERS];
};
// ring capacity must be power of 2


}
