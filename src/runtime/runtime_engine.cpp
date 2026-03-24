#include "runtime_engine.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include <immintrin.h>  // _mm_pause


template<std::size_t WindowSize>
BurstDetector<WindowSize>::BurstDetector(BurstCfg cfg)
    : burst_threshold_ns_(cfg.burst_threshold_ns),
      burst_exit_threshold_ns_(cfg.burst_exit_threshold_ns) {}

template<std::size_t WindowSize>
BurstDetector<WindowSize>::BurstDetector()
    : BurstDetector(BurstCfg{
        .burst_threshold_ns      = 10'000,   // enter burst if mean inter-arrival < 10us
        .burst_exit_threshold_ns = 50'000,   // exit burst if mean inter-arrival > 50us
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
        // evict oldest delta: between timestamps_[head_] and its successor
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

// explicit instantiation for the default used by RuntimeEngine
template class BurstDetector<16>;



TickProcessor::TickProcessor(double alpha, int print_every)
    : alpha_(alpha), print_every_(print_every) {}

void TickProcessor::on_tick(const CachedTick& tick) {
    if (first_) {
        ema_ = tick.price;
        first_ = false;
        window_start_ns_ = tick.received_at_ns;
    } else {
        ema_ = alpha_ * tick.price + (1.0 - alpha_) * ema_;
    }

    ++tick_count_;
    ++ticks_in_window_;

    uint64_t elapsed_ns = tick.received_at_ns - window_start_ns_;
    if (elapsed_ns >= 1'000'000'000ULL) {
        tick_rate_ = static_cast<double>(ticks_in_window_) * 1e9 / static_cast<double>(elapsed_ns);
        ticks_in_window_ = 0;
        window_start_ns_ = tick.received_at_ns;
    }

    if (tick_count_ % print_every_ == 0) {
        std::cout << "seq=" << tick.sequence_number
                  << " price=" << tick.price
                  << " ema=" << ema_
                  << " rate=" << tick_rate_ << " ticks/s\n";
    }
}


void RuntimeEngine::run(volatile sig_atomic_t& running) {
    std::cout << "Waiting for shared memory...\n";
    while (!shm_.open()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Attached to shared memory. Polling...\n";

    auto* block = shm_.as<engine::shm::SharedMemoryBlock>();
    uint64_t last_seen_seq = 0;

    while (running) {
        uint64_t seq = block->latest_market_data.sequence_number.load(std::memory_order_acquire);
        if (seq == last_seen_seq) {
            _mm_pause();
            continue;
        }

        double   price       = block->latest_market_data.price.load(std::memory_order_relaxed);
        uint64_t ts          = block->latest_market_data.timestamp.load(std::memory_order_relaxed);
        uint64_t received_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        CachedTick tick{seq, price, ts, received_at};
        cache_.push(tick);
        processor_.on_tick(tick);
        detector_.on_tick(received_at);
        last_seen_seq = seq;

        bool burst_now = detector_.in_burst();

        if (burst_now) {
            uint64_t delta = (prev_tick_received_at_ > 0)
                ? received_at - prev_tick_received_at_ : 0;

            if (pending_count_ < engine::shm::BATCH_SIZE) {
                pending_batch_[pending_count_++] = engine::shm::AcceleratorTick{
                    .sequence_number  = seq,
                    .price            = price,
                    .arrival_delta_ns = delta,
                };
            }

            if (!was_in_burst_) {
                // record burst entry metadata; flush happens at burst end or batch full
                auto& batch = block->data_to_accelerator;
                batch.burst_meta.burst_entry_time_ns = received_at;
                batch.burst_meta.ema_at_entry        = processor_.ema();
            }

            if (pending_count_ == engine::shm::BATCH_SIZE) {
                auto& batch = block->data_to_accelerator;
                batch.burst_meta.tick_count = pending_count_;
                for (uint32_t i = 0; i < pending_count_; ++i)
                    batch.ticks[i] = pending_batch_[i];
                batch.count = pending_count_;
                batch.batch_sequence_number.store(++last_batch_seq_, std::memory_order_release);
                block->accelerator_signal.routing_active.store(true, std::memory_order_release);

                auto s = detector_.stats();
                std::cout << "[ROUTE] seq=" << seq
                          << " batch_size=" << pending_count_
                          << " burst_ticks=" << s.burst_tick_count
                          << " ema_at_entry=" << batch.burst_meta.ema_at_entry << '\n';

                pending_count_ = 0;
            }
        } else {
            if (was_in_burst_) {
                auto s = detector_.stats();

                if (pending_count_ > 0) {
                    auto& batch = block->data_to_accelerator;
                    batch.burst_meta.tick_count = pending_count_;
                    for (uint32_t i = 0; i < pending_count_; ++i)
                        batch.ticks[i] = pending_batch_[i];
                    batch.count = pending_count_;
                    batch.batch_sequence_number.store(++last_batch_seq_, std::memory_order_release);
                    block->accelerator_signal.routing_active.store(true, std::memory_order_release);

                    std::cout << "[ROUTE] seq=" << seq
                              << " batch_size=" << pending_count_
                              << " burst_ticks=" << s.burst_tick_count
                              << " ema_at_entry=" << batch.burst_meta.ema_at_entry << '\n';
                    pending_count_ = 0;
                }

                std::cout << "[ROUTE END] burst_ticks=" << s.burst_tick_count
                          << " mean_inter_arrival_ns=" << s.mean_inter_arrival_ns << '\n';
            }

            // collect any pending accelerator response
            uint64_t result_seq = block->accelerator_signal.result_sequence_number
                                      .load(std::memory_order_acquire);
            if (result_seq > last_result_seq_) {
                last_result_seq_ = result_seq;
                double  proc_ema = block->accelerator_signal.processed_ema.load(std::memory_order_relaxed);
                int8_t  action   = block->accelerator_signal.signal_action.load(std::memory_order_relaxed);
                std::cout << "[ACCEL] result_seq=" << result_seq
                          << " processed_ema=" << proc_ema
                          << " action=" << static_cast<int>(action) << '\n';
            }
        }

        was_in_burst_          = burst_now;
        prev_tick_received_at_ = received_at;
    }
}
