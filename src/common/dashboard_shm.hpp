#pragma once
 
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>

#include "price_level.hpp"

namespace dashboard::shm {
// Buffer 4: gui seqlock buffer. owned by dashboard
// PRODUCER: Runtime engine cool thread
// CONSUMER: dashboard processed_ema
inline constexpr std::size_t SNAPSHOT_DEPTH = 16;

struct alignas(64) BookSnapshot {

    std::atomic<uint64_t> version{0};

    int64_t best_bid{0};
    int64_t best_ask{0};
    uint64_t total_bid_qty{0};
    uint64_t total_ask_qty{0};

    int64_t spread{0};
    double imbalance{0.0};
    double vwmid{0};
    int64_t ema{0};
    
    double tick_rate{0.0};
    bool in_burst{false};

    PriceLevel bids[SNAPSHOT_DEPTH];
    PriceLevel asks[SNAPSHOT_DEPTH];
    uint32_t bid_level_count{0};
    uint32_t ask_level_count{0};

    uint64_t event_sequence{0};

    double latency_p50_us{0.0};       // rolling P50 E2E latency (µs)
    double latency_p99_us{0.0};       // rolling P99 E2E latency (µs)
    double ring_occupancy{0.0};       // SPSC ring fill ratio [0.0, 1.0]
    double burst_threshold_tps{0.0};  // tick rate above which bursts offload to accelerator
};


inline void snapshot_begin_write(BookSnapshot &s) noexcept {
    s.version.fetch_add(1, std::memory_order_release);
}

inline void snapshot_end_write(BookSnapshot &s) noexcept {
    s.version.fetch_add(1, std::memory_order_release);
}

// attempts to memcpy the booksnapshot to the display thread 64 times before returning false on an invalid state
inline bool snapshot_read(const BookSnapshot &src, BookSnapshot &out) noexcept {
    for (int i = 0; i < 64; i++ ) {
        uint64_t v1 = src.version.load(std::memory_order_acquire);
        if (v1 & 1u) continue; //spin on odd version
        std::memcpy(
                reinterpret_cast<char*>(&out) + sizeof(std::atomic<uint64_t>),
                reinterpret_cast<const char*>(&src) + sizeof(std::atomic<uint64_t>),
                sizeof(BookSnapshot) - sizeof(std::atomic<uint64_t>)
            );
        uint64_t v2 = src.version.load(std::memory_order_acquire);
        if (v1 == v2) return v1 > 0; //v1 = 0 => no book to write
    }
    return false;
}

}
