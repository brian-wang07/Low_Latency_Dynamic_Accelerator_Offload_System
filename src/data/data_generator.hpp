#pragma once


#include <cstdint>
#include <cstddef>
#include <random>

struct MarketDataSnapshot {
    uint64_t sequence_number;
    double price;
    uint64_t timestamp;

};


struct GeneratedTick {
    MarketDataSnapshot data;
    uint64_t wait_ns;
    bool in_burst;
};

struct DataGeneratorConfig {
    std::uint64_t base_interval_ns;
    std::uint64_t burst_interval_ns;
    std::size_t   burst_length;
    double        start_price;
    double        volatility;
    double        burst_probability;
    double        burst_volatility_multiplier;
    std::uint64_t seed;
    double        jitter_sigma;
    bool          enable_bursts;

};

///generates next tick. seperate from shm
class DataGenerator {
public:
    explicit DataGenerator(DataGeneratorConfig cfg);
    GeneratedTick next();

private:
    DataGeneratorConfig cfg_;
    std::mt19937_64 rng_;

    std::uint64_t sequence_ = 0;
    std::size_t burst_remaining_ = 0;
    std::normal_distribution<double> dist_{0.0, 1.0}; //mean 0, stddev 1
    double current_price_ = 0.0;
    bool in_burst_ = false;

};