#include "runtime_engine.hpp"

#include <cstdio>
#include "spin_pause.hpp"


template<std::size_t WindowSize>
BurstDetector<WindowSize>::BurstDetector(BurstCfg cfg)
    : burst_threshold_ns_(cfg.burst_threshold_ns),
      burst_exit_threshold_ns_(cfg.burst_exit_threshold_ns) {}

template<std::size_t WindowSize>
BurstDetector<WindowSize>::BurstDetector()
    : BurstDetector(BurstCfg{
        .burst_threshold_ns      = 20,
        .burst_exit_threshold_ns = 35,
      }) {}

template<std::size_t WindowSize>
void BurstDetector<WindowSize>::on_tick(uint64_t received_at_ns) {
    if (filled_ == 0) {
        timestamps_[head_] = received_at_ns;
        head_   = (head_ + 1) % WindowSize;
        filled_ = 1;
        return;
    }

    uint64_t prev      = timestamps_[(head_ + WindowSize - 1) % WindowSize];
    uint64_t new_delta = received_at_ns - prev;

    if (filled_ == WindowSize) {
        uint64_t evicted = timestamps_[(head_ + 1) % WindowSize] - timestamps_[head_];
        running_sum_ -= evicted;
    } else {
        ++filled_;
    }

    running_sum_ += new_delta;
    timestamps_[head_] = received_at_ns;
    head_ = (head_ + 1) % WindowSize;

    uint64_t mean = running_sum_ / (filled_ - 1);

    if (!in_burst_ && mean < burst_threshold_ns_) {
        in_burst_         = true;
        burst_entry_ns_   = received_at_ns;
        burst_tick_count_ = 1;
    } else if (in_burst_) {
        ++burst_tick_count_;
        if (mean > burst_exit_threshold_ns_)
            in_burst_ = false;
    }
}

template<std::size_t WindowSize>
bool BurstDetector<WindowSize>::in_burst() const noexcept {
    return in_burst_;
}

template<std::size_t WindowSize>
BurstStats BurstDetector<WindowSize>::stats() const noexcept {
    uint64_t mean = (filled_ > 1) ? running_sum_ / (filled_ - 1) : 0;
    return BurstStats{
        .mean_inter_arrival_ns = mean,
        .burst_entry_ns        = burst_entry_ns_,
        .burst_tick_count      = burst_tick_count_,
    };
}

template class BurstDetector<16>;


TickProcessor::TickProcessor(double alpha) : alpha_(alpha) {}

void TickProcessor::on_tick(int64_t mid, uint64_t received_at_ns) {
    if (first_) {
        ema_ = mid;
        first_ = false;
        window_start_ns_ = received_at_ns;
    } else {
        ema_ = static_cast<int64_t>(alpha_ * mid + (1.0 - alpha_) * ema_);
    }

    ++ticks_in_window_;

    uint64_t elapsed_ns = received_at_ns - window_start_ns_;
    if (elapsed_ns >= 1'000'000'000ULL) {
        tick_rate_ = static_cast<double>(ticks_in_window_) * 1e9 / static_cast<double>(elapsed_ns);
        ticks_in_window_ = 0;
        window_start_ns_ = received_at_ns;
    }
}



