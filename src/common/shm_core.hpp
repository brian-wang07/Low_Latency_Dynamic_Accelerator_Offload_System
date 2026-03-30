#pragma once

#include <cstdint>
#include <cstddef>

namespace common::shm {
    
// 16 MB
inline constexpr const char*   SHM_NAME = "/engine_shm_mvp";
inline constexpr std::size_t   SHM_SIZE = 16 * 1024 * 1024;
inline constexpr std::uint32_t SHM_MAGIC = 0xDEADBEEF; //version validation
inline constexpr std::uint32_t SHM_VERSION = 4;


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

    uint8_t source_id;        // 0 = dispatcher, 1 = strategy, 2+ = adversary

    uint8_t  _pad[5];
};
static_assert(sizeof(ShmOrderEvent) == 64);


struct alignas(64) ShmHeader {
    uint32_t magic;
    uint32_t version;
};



}
