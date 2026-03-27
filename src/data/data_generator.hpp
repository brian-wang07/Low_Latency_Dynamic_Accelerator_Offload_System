#pragma once

// Generative model for synthetic order event streams.
// Maintains an internal book (price-level depth + live order map) for cancel
// selection, market-order matching, and price-impact computation.
// BBO is derived from the OU mid + CIR spread, not from the book.

#include <random>
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <queue>

#include "config.hpp"

struct LiveOrder {
    uint64_t order_id;
    Side     side;
    int64_t  price;
    int64_t  qty;
    double   expiry_time;
};

struct ExpiryEntry {
    double   expiry_time;
    uint64_t order_id;
    bool operator>(const ExpiryEntry& o) const { return expiry_time > o.expiry_time; }
};

class DataGenerator {
public:
    explicit DataGenerator(DataGeneratorConfig cfg);
    GeneratedEvent next();

private:
    DataGeneratorConfig cfg_;
    std::mt19937_64 rng_;
    std::normal_distribution<double>     normal_{0.0, 1.0};
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};

    uint64_t next_order_id_ = 1;
    double   current_time_  = 0.0;
    double   mu_;
    double   spread_;

    bool     in_burst_        = false;
    size_t   burst_remaining_ = 0;

    std::vector<double> hawkes_bid_times_;
    std::vector<double> hawkes_ask_times_;
    double   pending_hawkes_time_;
    Side     pending_hawkes_side_;

    std::map<int64_t, int64_t, std::greater<int64_t>> bid_levels_;
    std::map<int64_t, int64_t>                         ask_levels_;
    std::unordered_map<uint64_t, LiveOrder>             live_orders_;
    std::priority_queue<ExpiryEntry, std::vector<ExpiryEntry>,
                        std::greater<ExpiryEntry>>      expiry_queue_;

    std::deque<GeneratedEvent> pending_events_;

    int64_t  to_fixed(double price) const;
    uint64_t time_to_ns(double t) const;
    uint64_t apply_jitter(uint64_t ps);
    void     update_burst();
    int64_t  compute_best_bid() const;
    int64_t  compute_best_ask() const;
    double   hawkes_intensity_at(Side side, double t) const;
    void     generate_next_hawkes();
    void     prune_hawkes_history();
    void     step_processes(double dt);
    int64_t  sample_limit_price(Side side);
    int64_t  sample_marketable_limit_price(Side side);
    int64_t  sample_qty();
    void     add_live_order(const LiveOrder& order);
    void     remove_live_order(uint64_t order_id);
    void     match_market_order(Side aggressor_side, int64_t qty, uint64_t ts_ns);
    int64_t  match_limit_order(Side aggressor_side, int64_t limit_price,
                               int64_t qty, uint64_t ts_ns);
    void     apply_price_impact(Side aggressor_side, int64_t fill_qty);
    GeneratedEvent make_event(OrderEvent ev, uint64_t wait_ps) const;
};
