#pragma once

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>

#include "shm_manager.hpp"
#include "shm_types.hpp"

struct CachedTick {
    uint64_t sequence_number;
    double   price;
    uint64_t data_timestamp_ns;  // from MarketData::timestamp
    uint64_t received_at_ns;     // steady_clock::now() at observation
};

struct BurstStats {
    uint64_t mean_inter_arrival_ns;
    uint64_t burst_entry_ns;
    uint64_t burst_tick_count;
};

struct BurstCfg {
    uint64_t burst_threshold_ns;
    uint64_t burst_exit_threshold_ns;
};


template<typename T, std::size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
public:
    void push(const T& item) {
        buf_[head_ & (N - 1)] = item;
        ++head_;
    }
    std::size_t count() const noexcept { return head_ < N ? head_ : N; }
    const T& operator[](std::size_t i) const noexcept { return buf_[i & (N - 1)]; }
private:
    std::array<T, N> buf_{};
    std::size_t head_ = 0;
};

class TickProcessor {
public:
    explicit TickProcessor(double alpha = 0.05, int print_every = 1000);
    void on_tick(const CachedTick& tick);
    double ema() const noexcept { return ema_; }

private:
    double   alpha_;
    int      print_every_;
    double   ema_ = 0.0;
    bool     first_ = true;
    int      tick_count_ = 0;
    uint64_t window_start_ns_ = 0;
    int      ticks_in_window_ = 0;
    double   tick_rate_ = 0.0;
};

template<std::size_t WindowSize = 16>
class BurstDetector {
public:
    explicit BurstDetector(BurstCfg cfg);
    BurstDetector();

    void on_tick(uint64_t received_at_ns);
    bool in_burst() const noexcept;
    BurstStats stats() const noexcept;

private:
    std::array<uint64_t, WindowSize> timestamps_{};
    bool        in_burst_ = false;
    std::size_t head_   = 0;
    std::size_t filled_ = 0;  // capped at WindowSize once full
    uint64_t    burst_threshold_ns_;
    uint64_t    burst_exit_threshold_ns_;
    uint64_t    burst_entry_ns_    = 0;
    uint64_t    burst_tick_count_  = 0;
    uint64_t    running_sum_       = 0;
};


class RuntimeEngine {
public:
    void run(volatile sig_atomic_t& running);

private:
    ShmManager shm_;
    RingBuffer<CachedTick, 1024> cache_;
    TickProcessor processor_;
    BurstDetector<> detector_;

    // batch accumulation
    engine::shm::AcceleratorTick pending_batch_[engine::shm::BATCH_SIZE]{};
    uint32_t pending_count_        = 0;
    uint64_t last_batch_seq_       = 0;
    uint64_t last_result_seq_      = 0;
    uint64_t prev_tick_received_at_ = 0;
    bool     was_in_burst_         = false;
};
