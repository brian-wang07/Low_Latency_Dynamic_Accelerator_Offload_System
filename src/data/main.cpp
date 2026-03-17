#include <iostream>
#include <chrono>
#include <random>
#include <cmath>
#include <thread>
#include <algorithm>

#include "data_generator.hpp"



DataGenerator::DataGenerator(DataGeneratorConfig cfg) :
    cfg_(std::move(cfg)),
    rng_(cfg_.seed),
    current_price_(cfg_.start_price) {}


GeneratedTick DataGenerator::next() {
    uint64_t current_interval = cfg_.base_interval_ns;
    double current_volatility = cfg_.volatility;

    if (cfg_.enable_bursts) {
        if (!in_burst_) {
            std::bernoulli_distribution burst_start_dist(cfg_.burst_probability);
            if (burst_start_dist(rng_)) {
                in_burst_ = true;
                burst_remaining_ = cfg_.burst_length;
            }
        }

        if (in_burst_) {
            current_interval = cfg_.burst_interval_ns;
            current_volatility *= cfg_.burst_volatility_multiplier;

            if (burst_remaining_ > 0) {
                --burst_remaining_;
            }
            if (burst_remaining_ == 0) {
                in_burst_ = false;
            }
        }
    }

    //simulate jitter
    std::lognormal_distribution<double> log_dist(0.0, cfg_.jitter_sigma);
    double wait_multiplier = log_dist(rng_);
    uint64_t actual_wait_ns = std::max<uint64_t>(current_interval * wait_multiplier, 1);


    double time_scaling = std::sqrt(static_cast<double>(actual_wait_ns) / static_cast<double>(current_interval));
    double price_change = dist_(rng_) * current_volatility * time_scaling;

    current_price_ += price_change;

    current_price_ = std::max(current_price_, 0.01);


    uint64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    MarketDataSnapshot data {
        .sequence_number = ++sequence_, 
        .price = current_price_,
        .timestamp = timestamp
    };

    return GeneratedTick {
        .data = data,
        .wait_ns = actual_wait_ns,
        .in_burst = in_burst_
    };


}

int main() {
    DataGeneratorConfig cfg{
        .base_interval_ns = 100'000,          // 0.1 ms
        .burst_interval_ns = 1'000,           // 0.001 ms
        .burst_length = 5,
        .start_price = 100.0,
        .volatility = 0.5,
        .burst_probability = 0.1,
        .burst_volatility_multiplier = 3.0,
        .seed = 42,
        .jitter_sigma = 0.2,
        .enable_bursts = true
    };

    DataGenerator generator(cfg);
    int i = 0;
    using clock = std::chrono::steady_clock;
    auto next_time = clock::now();
    while (++i) {
        auto tick = generator.next();

        std::cout
            << "seq=" << tick.data.sequence_number
            << " price=" << tick.data.price
            << " wait_ms=" << tick.wait_ns / 1000000.0
            << " burst=" << tick.in_burst
            << '\n';

        next_time += std::chrono::nanoseconds(tick.wait_ns);

        // hybrid wait
        while (true) {
            auto now = clock::now();
            if (now >= next_time) break;

            auto remaining = next_time - now;

            if (remaining > std::chrono::microseconds(50)) {
                std::this_thread::sleep_for(remaining - std::chrono::microseconds(20));
            }
        }
    }

    return 0;
}