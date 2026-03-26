#pragma once

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>

#include "shm_manager.hpp"
#include "shm_types.hpp"
#include "order_book.hpp"

struct BurstStats {
    uint64_t mean_inter_arrival_ns;
    uint64_t burst_entry_ns;
    uint64_t burst_tick_count;
};

struct BurstCfg {
    uint64_t burst_threshold_ns;
    uint64_t burst_exit_threshold_ns;
};

class TickProcessor {
public:
    explicit TickProcessor(double alpha = 0.05);
    void on_tick(int64_t mid, uint64_t received_at_ns);
    int64_t ema() const noexcept { return ema_; }
    double  tick_rate() const noexcept { return tick_rate_; }

private:
    double   alpha_;
    int64_t  ema_ = 0;
    bool     first_ = true;
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
    std::size_t filled_ = 0;
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
    TickProcessor processor_;
    BurstDetector<> detector_;
    OrderBook book_;

    // ring consumer state
    uint64_t last_ring_tail_ = 0;

    // accelerator batch accumulation
    engine::shm::AcceleratorTick pending_batch_[engine::shm::BATCH_SIZE]{};
    uint32_t pending_count_         = 0;
    uint64_t last_batch_seq_        = 0;
    uint64_t last_result_seq_       = 0;
    uint64_t prev_tick_received_at_ = 0;
    bool     was_in_burst_          = false;

    void flush_batch(engine::shm::SharedMemoryBlock* block, uint64_t seq);
    void print_book() const;
};
