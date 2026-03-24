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
            << " wait_ms=" << tick.wait_ns
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