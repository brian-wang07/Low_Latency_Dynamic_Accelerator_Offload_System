#pragma once


#include <cstdint>
#include <cstddef>
#include <random>

enum class OrderType : uint8_t {
    LIMIT  = 0,
    MARKET = 1,
};

enum class Side : uint8_t {
    BID = 0,
    ASK = 1,
};

enum class OrderAction: uint8_t {
    NEW    = 0,
    CANCEL = 1,
    MODIFY = 2,
};

struct MarketDataSnapshot {
    uint64_t    sequence_number;
    double      bid; //best bid
    double      ask; //best ask
    uint32_t    depth;
    double      price;
    OrderType   order_type;
    Side        side;
    OrderAction action;
    uint64_t    timestamp;

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
    double        drift;         // annualized drift for mid GBM (e.g. 2e-6 ≈ +1 per 5000 ticks at price 100)
    double        burst_probability;
    double        burst_volatility_multiplier;
    std::uint64_t seed;
    double        jitter_sigma;
    bool          enable_bursts;

    double        spread_mean;
    double        spread_reversion_speed;
    double        spread_volatility;
    double        min_spread;
    double        start_spread;

    double        depth_log_mean;
    double        depth_log_sigma;

    double        prob_limit; // P(LIMIT), rest MARKET
    double        prob_bid; // P(BID), rest ASK
    double        prob_new; // P(NEW)
    double        prob_cancel; // P(CANCEL), rest MODIFY
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
    std::lognormal_distribution<double> depth_dist_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    double current_price_;   // mid price (GBM)
    double current_spread_;  // spread (OU)
    bool in_burst_ = false;

};