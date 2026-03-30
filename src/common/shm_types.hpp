#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "engine_types.hpp"

namespace engine::shm {

inline constexpr const char*   SHM_NAME = "/engine_shm_mvp";
inline constexpr std::size_t   SHM_SIZE = 1024 * 1024; // 1MB for MVP
inline constexpr std::uint32_t SHM_MAGIC = 0x454E474E; //version validation
inline constexpr std::uint32_t SHM_VERSION = 3;

inline constexpr std::uint8_t  BATCH_SIZE          = 64;
inline constexpr std::uint32_t EVENT_RING_CAPACITY = 8192;
inline constexpr std::uint32_t EVENT_RING_MASK     = EVENT_RING_CAPACITY - 1;
static_assert((EVENT_RING_CAPACITY & (EVENT_RING_CAPACITY - 1)) == 0, "must be power of 2");

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


struct alignas(64) ShmOrderEvent {
    uint64_t sequence;        // monotonic; 0 = unwritten
    uint64_t timestamp_ns;    // simulated Hawkes process time (not wall clock)
    uint64_t enqueue_tsc;     // TSC cycles (READ_TSC) at ring write — for latency measurement
    uint64_t order_id;
    int64_t  price;           // the ORDER's quoted price, fixed-point × PRICE_SCALE
                              // (limit price for ADD_LIMIT/CANCEL, fill price for EXECUTE, 0 for ADD_MARKET)
    int64_t  qty;
    int64_t  qty_remaining;
    uint8_t  type;            // EventType enum
    uint8_t  side;            // Side enum
    uint8_t  _pad[6];
};
static_assert(sizeof(ShmOrderEvent) == 64);

struct alignas(64) EventRingBuffer {
    alignas(64) std::atomic<uint64_t> head{0};  // next slot producer will write (producer-owned)
    alignas(64) std::atomic<uint64_t> tail{0};  // next slot consumer will read (consumer-owned)
    alignas(64) ShmOrderEvent slots[EVENT_RING_CAPACITY];
};

struct alignas(64) ShmHeader {
    uint32_t magic;
    uint32_t version;
};

struct SharedMemoryBlock {
    alignas(64) ShmHeader         header;
    alignas(64) AcceleratorBatch  data_to_accelerator; // runtime -> accelerator
    alignas(64) AcceleratorSignal accelerator_signal;   // accelerator -> runtime
    alignas(64) EventRingBuffer   event_ring;           // data -> runtime
};

static_assert(offsetof(SharedMemoryBlock, data_to_accelerator) % 64 == 0);
static_assert(offsetof(SharedMemoryBlock, accelerator_signal) % 64 == 0);
static_assert(offsetof(SharedMemoryBlock, event_ring) % 64 == 0);
static_assert(sizeof(SharedMemoryBlock) <= SHM_SIZE);
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

