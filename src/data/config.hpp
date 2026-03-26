#pragma once

#include <cstdint>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include "json.hpp"

#include "../common/engine_types.hpp"

struct OrderEvent {
    uint64_t  timestamp_ns;
    uint64_t  order_id;
    EventType type;
    Side      side;
    int64_t   price;         // fixed-point (× PRICE_SCALE); 0 for market orders
    int64_t   qty;
    int64_t   qty_remaining;
};

struct GeneratedEvent {
    OrderEvent event;
    int64_t    mid;          // current mid price, fixed-point
    int64_t    best_bid;
    int64_t    best_ask;
    int64_t    depth_bid;    // total qty at best bid level
    int64_t    depth_ask;
    uint64_t   wait_ps;      // inter-event time (picoseconds)
    bool       in_burst;
};

// User-facing knobs — all fields are required in config.json.
struct StreamConfig {
    uint64_t base_interval_ns            = 50;    // avg ns between events (normal mode)
    uint64_t burst_interval_ns           = 5;     // avg ns between events (burst mode)
    double   daily_volatility            = 0.08;  // daily price vol as fraction
    double   mean_reversion_speed        = 0.01;  // OU κ — how fast price reverts to μ₀
    double   burst_probability           = 0.10;  // chance a burst starts on each event
    double   burst_volatility_multiplier = 6.0;   // vol multiplier during burst
    uint64_t seed                        = 42;
};

// Full internal parameter set — derived from StreamConfig via make_generator_config().
// Optional fields in config.json override individual values after derivation.
struct DataGeneratorConfig {
    // OU mid-price process
    double   mu_0            = 95000.0;
    double   kappa           = 0.01;
    double   sigma           = 0.08;
    double   tick_size       = 0.1;

    // CIR spread process
    double   kappa_s         = 5.0;
    double   theta_s         = 1.0;   // long-run mean spread (ticks)
    double   xi              = 0.15;
    double   start_spread    = 1.0;

    // Hawkes process
    double   hawkes_mu0      = 2500.0;
    double   hawkes_alpha    = 0.6;
    double   hawkes_beta     = 8.0;

    // Event type probabilities
    double   p_add_limit     = 0.72;
    double   p_cancel        = 0.21;
    double   p_add_market    = 0.06;
    double   p_modify        = 0.01;

    double   cancel_rate_gamma = 25000.0;  // exponential lifetime parameter

    // Price placement — truncated geometric
    double   geom_p           = 0.45;
    int      max_offset_ticks = 5;

    // Order quantity — log-normal
    double   qty_log_mean     = 5.298;
    double   qty_log_sigma    = 0.6;
    int      lot_size         = 1;

    double   impact_coeff     = 0.008;  // price impact feedback strength

    // Burst mode
    bool     enable_bursts               = true;
    double   burst_probability           = 0.10;
    size_t   burst_length                = 10;
    double   burst_rate_multiplier       = 10.0;
    double   burst_volatility_multiplier = 6.0;

    double   jitter_sigma     = 0.3;    // lognormal inter-event jitter

    uint64_t seed             = 42;
};

inline DataGeneratorConfig make_generator_config(const StreamConfig& sc) {
    DataGeneratorConfig cfg{};
    cfg.hawkes_mu0            = 1e9 / static_cast<double>(sc.base_interval_ns) / 2.0;
    cfg.burst_rate_multiplier = static_cast<double>(sc.base_interval_ns)
                              / static_cast<double>(sc.burst_interval_ns);
    cfg.sigma                 = sc.daily_volatility * cfg.mu_0 / std::sqrt(86400.0);
    cfg.kappa                 = sc.mean_reversion_speed;
    cfg.burst_probability     = sc.burst_probability;
    cfg.burst_volatility_multiplier = sc.burst_volatility_multiplier;
    cfg.enable_bursts         = true;
    cfg.seed                  = sc.seed;
    return cfg;
}

// Load config from JSON. StreamConfig fields are required; DataGeneratorConfig
// fields are optional overrides applied after make_generator_config().
inline DataGeneratorConfig load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config file: " + path);
    auto j = nlohmann::json::parse(f);

    StreamConfig sc;
    sc.base_interval_ns            = j.at("base_interval_ns");
    sc.burst_interval_ns           = j.at("burst_interval_ns");
    sc.daily_volatility            = j.at("daily_volatility");
    sc.mean_reversion_speed        = j.at("mean_reversion_speed");
    sc.burst_probability           = j.at("burst_probability");
    sc.burst_volatility_multiplier = j.at("burst_volatility_multiplier");
    sc.seed                        = j.at("seed");

    DataGeneratorConfig cfg = make_generator_config(sc);

    auto opt = [&](const char* key, auto& field) {
        if (j.contains(key)) field = j[key];
    };
    opt("mu_0",                    cfg.mu_0);
    opt("kappa",                   cfg.kappa);
    opt("sigma",                   cfg.sigma);
    opt("tick_size",               cfg.tick_size);
    opt("kappa_s",                 cfg.kappa_s);
    opt("theta_s",                 cfg.theta_s);
    opt("xi",                      cfg.xi);
    opt("start_spread",            cfg.start_spread);
    opt("hawkes_mu0",              cfg.hawkes_mu0);
    opt("hawkes_alpha",            cfg.hawkes_alpha);
    opt("hawkes_beta",             cfg.hawkes_beta);
    opt("p_add_limit",             cfg.p_add_limit);
    opt("p_cancel",                cfg.p_cancel);
    opt("p_add_market",            cfg.p_add_market);
    opt("p_modify",                cfg.p_modify);
    opt("cancel_rate_gamma",       cfg.cancel_rate_gamma);
    opt("geom_p",                  cfg.geom_p);
    opt("max_offset_ticks",        cfg.max_offset_ticks);
    opt("qty_log_mean",            cfg.qty_log_mean);
    opt("qty_log_sigma",           cfg.qty_log_sigma);
    opt("lot_size",                cfg.lot_size);
    opt("impact_coeff",            cfg.impact_coeff);
    opt("enable_bursts",           cfg.enable_bursts);
    opt("burst_length",            cfg.burst_length);
    opt("burst_rate_multiplier",   cfg.burst_rate_multiplier);
    opt("jitter_sigma",            cfg.jitter_sigma);
    return cfg;
}
