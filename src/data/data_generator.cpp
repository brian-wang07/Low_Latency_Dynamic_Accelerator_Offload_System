#include <chrono>
#include <random>
#include <cmath>
#include <algorithm>

#include "data_generator.hpp"

DataGenerator::DataGenerator(DataGeneratorConfig cfg) :
    cfg_(std::move(cfg)),
    rng_(cfg_.seed),
    current_price_(cfg_.start_price),
    current_spread_(cfg_.start_spread),
    depth_dist_(cfg_.depth_log_mean, cfg_.depth_log_sigma) {}


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
    double dt           = time_scaling * time_scaling;
    double dW           = dist_(rng_) * time_scaling;

    // convert absolute volatility (dollar terms) to fractional for GBM
    // use start_price (not current_price_) to avoid state-dependent volatility feedback loop:
    // if price drops → frac_vol rises → variance rises → price drops further → NaN
    double frac_vol     = current_volatility / cfg_.start_price;
    double log_return   = (cfg_.drift - 0.5 * frac_vol * frac_vol) * dt
                        + frac_vol * dW;
    current_price_     *= std::exp(log_return);

    // spread: OU mean-reversion + independent noise, floored at min_spread
    double dW2           = dist_(rng_) * time_scaling;
    double spread_change = cfg_.spread_reversion_speed * (cfg_.spread_mean - current_spread_) * dt
                         + cfg_.spread_volatility * dW2;
    current_spread_ = std::max(current_spread_ + spread_change, cfg_.min_spread);

    double bid = current_price_ - current_spread_ / 2.0;
    double ask = current_price_ + current_spread_ / 2.0;

    uint32_t    depth      = std::max(1u, static_cast<uint32_t>(std::lround(depth_dist_(rng_))));
    Side        side       = uniform_(rng_) < cfg_.prob_bid   ? Side::BID    : Side::ASK;
    OrderType   order_type = uniform_(rng_) < cfg_.prob_limit ? OrderType::LIMIT : OrderType::MARKET;
    double      u          = uniform_(rng_);
    OrderAction action     = u < cfg_.prob_new                    ? OrderAction::NEW
                           : u < cfg_.prob_new + cfg_.prob_cancel ? OrderAction::CANCEL
                                                                   : OrderAction::MODIFY;

    uint64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    MarketDataSnapshot data {
        .sequence_number = ++sequence_,
        .bid        = bid,
        .ask        = ask,
        .depth      = depth,
        .price      = current_price_,
        .order_type = order_type,
        .side       = side,
        .action     = action,
        .timestamp  = timestamp
    };

    return GeneratedTick {
        .data     = data,
        .wait_ns  = actual_wait_ns,
        .in_burst = in_burst_
    };
}
