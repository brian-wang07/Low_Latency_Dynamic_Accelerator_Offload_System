#include "runtime_engine.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include <immintrin.h>  // _mm_pause

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

void RuntimeEngine::run() {
    std::cout << "Waiting for shared memory...\n";
    while (!shm_.open()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Attached to shared memory. Polling...\n";

    auto* block = shm_.as<engine::shm::SharedMemoryBlock>();
    uint64_t last_seen_seq = 0;

    while (true) {
        uint64_t seq = block->latest_market_data.sequence_number.load(std::memory_order_acquire);
        if (seq == last_seen_seq) {
            _mm_pause();
            continue;
        }

        // Safe to read relaxed: seq_number was stored last with release,
        // so the acquire above synchronizes the entire prior store sequence.
        double   price = block->latest_market_data.price.load(std::memory_order_relaxed);
        uint64_t ts    = block->latest_market_data.timestamp.load(std::memory_order_relaxed);
        uint64_t received_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        CachedTick tick{seq, price, ts, received_at};
        cache_.push(tick);
        processor_.on_tick(tick);
        last_seen_seq = seq;
    }
}
