#include <iostream>
#include <chrono>
#include <cmath>
#include <thread>

#include "../data/data_generator.hpp"

int main() {
    DataGeneratorConfig cfg{
        .base_interval_ns = 100'000,          // 0.1 ms
        .burst_interval_ns = 1'000,           // 0.001 ms
        .burst_length = 5,
        .start_price = 100.0,
        .volatility = 0.5,
        .drift = 2e-6,
        .burst_probability = 0.1,
        .burst_volatility_multiplier = 3.0,
        .seed = 42,
        .jitter_sigma = 0.2,
        .enable_bursts = true,
        .spread_mean            = 0.10,
        .spread_reversion_speed = 5.0,
        .spread_volatility      = 0.02,
        .min_spread             = 0.01,
        .start_spread           = 0.10,
        .depth_log_mean         = 4.0,
        .depth_log_sigma        = 1.0,
        .prob_limit             = 0.70,
        .prob_bid               = 0.50,
        .prob_new               = 0.60,
        .prob_cancel            = 0.30,
    };

    DataGenerator generator(cfg);
    int i = 0;
    using clock = std::chrono::steady_clock;
    auto next_time = clock::now();
    while (++i) {
        auto tick = generator.next();

        std::cout
            << "seq="    << tick.data.sequence_number
            << " bid="   << tick.data.bid
            << " ask="   << tick.data.ask
            << " sprd="  << (tick.data.ask - tick.data.bid)
            << " depth=" << tick.data.depth
            << " type="  << (tick.data.order_type == OrderType::LIMIT ? "L" : "M")
            << " side="  << (tick.data.side == Side::BID ? "B" : "A")
            << " act="   << (tick.data.action == OrderAction::NEW ? "N"
                           : tick.data.action == OrderAction::CANCEL ? "C" : "D")
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