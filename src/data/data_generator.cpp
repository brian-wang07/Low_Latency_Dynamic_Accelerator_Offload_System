#include "data_generator.hpp"
#include <algorithm>
#include <cassert>

DataGenerator::DataGenerator(DataGeneratorConfig cfg)
    : cfg_(cfg)
    , rng_(cfg_.seed)
    , mu_(cfg_.mu_0)
    , spread_(cfg_.start_spread)
{
    generate_next_hawkes();
}

int64_t DataGenerator::to_fixed(double price) const {
    return static_cast<int64_t>(std::round(price * PRICE_SCALE));
}

uint64_t DataGenerator::time_to_ns(double t) const {
    return static_cast<uint64_t>(t * 1e9);
}

// BBO: best_bid = floor((μ − s/2) / tick) × tick,  best_ask = best_bid + round(s) × tick
int64_t DataGenerator::compute_best_bid() const {
    int64_t tick_fp = to_fixed(cfg_.tick_size);
    int64_t raw_fp  = to_fixed(mu_ - spread_ * cfg_.tick_size / 2.0);
    return (raw_fp / tick_fp) * tick_fp;
}

int64_t DataGenerator::compute_best_ask() const {
    int64_t spread_ticks = std::max(int64_t(1), static_cast<int64_t>(std::round(spread_)));
    return compute_best_bid() + spread_ticks * to_fixed(cfg_.tick_size);
}

uint64_t DataGenerator::apply_jitter(uint64_t ps) {
    if (cfg_.jitter_sigma <= 0.0 || ps == 0) return ps;
    double m = std::exp(normal_(rng_) * cfg_.jitter_sigma);
    return std::max(uint64_t(1), static_cast<uint64_t>(ps * m));
}

void DataGenerator::update_burst() {
    if (!cfg_.enable_bursts) return;
    if (in_burst_) {
        if (--burst_remaining_ == 0) in_burst_ = false;
    } else if (uniform_(rng_) < cfg_.burst_probability) {
        in_burst_        = true;
        burst_remaining_ = cfg_.burst_length;
    }
}

// Hawkes intensity: λ(t) = μ₀ + Σ α·exp(−β·(t − tᵢ))
double DataGenerator::hawkes_intensity_at(Side side, double t) const {
    double lam    = in_burst_ ? cfg_.hawkes_mu0 * cfg_.burst_rate_multiplier : cfg_.hawkes_mu0;
    double cutoff = 10.0 / cfg_.hawkes_beta;
    const auto& times = (side == Side::BID) ? hawkes_bid_times_ : hawkes_ask_times_;
    for (auto it = times.rbegin(); it != times.rend(); ++it) {
        double dt = t - *it;
        if (dt < 0) continue;
        if (dt > cutoff) break;   // sorted — remaining are older
        lam += cfg_.hawkes_alpha * std::exp(-cfg_.hawkes_beta * dt);
    }
    return lam;
}

// Ogata thinning: propose dt ~ Exp(λ̄), accept with prob λ(t)/λ̄
void DataGenerator::generate_next_hawkes() {
    double t       = current_time_;
    double lam_bar = hawkes_intensity_at(Side::BID, t)
                   + hawkes_intensity_at(Side::ASK, t);

    while (true) {
        double u1 = uniform_(rng_);
        if (u1 <= 0.0) u1 = 1e-15;
        t += -std::log(u1) / lam_bar;

        double lb  = hawkes_intensity_at(Side::BID, t);
        double la  = hawkes_intensity_at(Side::ASK, t);
        double lam = lb + la;

        if (uniform_(rng_) < lam / lam_bar) {
            pending_hawkes_side_ = (uniform_(rng_) < lb / lam) ? Side::BID : Side::ASK;
            pending_hawkes_time_ = t;
            return;
        }
        lam_bar = lam + cfg_.hawkes_alpha;  // tighten bound on reject
    }
}

void DataGenerator::prune_hawkes_history() {
    double cutoff = current_time_ - 10.0 / cfg_.hawkes_beta;
    auto prune = [cutoff](std::vector<double>& v) {
        auto it = std::lower_bound(v.begin(), v.end(), cutoff);
        if (it != v.begin()) v.erase(v.begin(), it);
    };
    prune(hawkes_bid_times_);
    prune(hawkes_ask_times_);
}

// OU mid:    μ(t+Δt) = μ + κ(μ₀ − μ)Δt + σ_eff·√Δt·Z
// CIR spread: s(t+Δt) = s + κₛ(θₛ − s)Δt + ξ·√s·√Δt·Z
void DataGenerator::step_processes(double dt) {
    if (dt <= 0) return;
    double sqrt_dt = std::sqrt(dt);

    double sigma_eff = in_burst_ ? cfg_.sigma * cfg_.burst_volatility_multiplier : cfg_.sigma;
    mu_ += cfg_.kappa * (cfg_.mu_0 - mu_) * dt + sigma_eff * sqrt_dt * normal_(rng_);

    double sqrt_s = std::sqrt(std::max(spread_, 0.0));
    spread_ += cfg_.kappa_s * (cfg_.theta_s - spread_) * dt
             + cfg_.xi * sqrt_s * sqrt_dt * normal_(rng_);
    spread_ = std::max(spread_, 1.0);  // hard reflect at 1 tick
}

// Truncated geometric price placement: δ ~ Geom(p) in [0, D]
// Bid: best_bid − δ·tick,  Ask: best_ask + δ·tick
int64_t DataGenerator::sample_limit_price(Side side) {
    int64_t tick_fp = to_fixed(cfg_.tick_size);
    double  u       = uniform_(rng_);
    double  q       = 1.0 - cfg_.geom_p;
    double  norm    = 1.0 - std::pow(q, cfg_.max_offset_ticks + 1);

    int delta = cfg_.max_offset_ticks;
    double cum = 0.0;
    for (int k = 0; k <= cfg_.max_offset_ticks; ++k) {
        cum += cfg_.geom_p * std::pow(q, k) / norm;
        if (u <= cum) { delta = k; break; }
    }

    return (side == Side::BID)
        ? compute_best_bid() - static_cast<int64_t>(delta) * tick_fp
        : compute_best_ask() + static_cast<int64_t>(delta) * tick_fp;
}

int64_t DataGenerator::sample_qty() {
    double  raw = std::exp(cfg_.qty_log_mean + cfg_.qty_log_sigma * normal_(rng_));
    int64_t qty = std::max(int64_t(1), static_cast<int64_t>(std::round(raw)));
    int64_t lot = cfg_.lot_size;
    return std::max(lot, ((qty + lot / 2) / lot) * lot);
}

void DataGenerator::add_live_order(const LiveOrder& order) {
    live_orders_[order.order_id] = order;
    if (order.side == Side::BID) bid_levels_[order.price] += order.qty;
    else                          ask_levels_[order.price] += order.qty;
}

void DataGenerator::remove_live_order(uint64_t order_id) {
    auto it = live_orders_.find(order_id);
    if (it == live_orders_.end()) return;
    auto& o = it->second;

    if (o.side == Side::BID) {
        auto lit = bid_levels_.find(o.price);
        if (lit != bid_levels_.end()) {
            lit->second -= o.qty;
            if (lit->second <= 0) bid_levels_.erase(lit);
        }
    } else {
        auto lit = ask_levels_.find(o.price);
        if (lit != ask_levels_.end()) {
            lit->second -= o.qty;
            if (lit->second <= 0) ask_levels_.erase(lit);
        }
    }
    live_orders_.erase(it);
}

// Walk opposite side from best level, emit EXECUTE events, apply price impact.
void DataGenerator::match_market_order(Side aggressor_side, int64_t qty, uint64_t ts_ns) {
    Side passive_side = (aggressor_side == Side::BID) ? Side::ASK : Side::BID;

    auto walk_levels = [&](auto& levels) {
        int64_t remaining    = qty;
        int64_t total_filled = 0;

        while (remaining > 0 && !levels.empty()) {
            auto    it          = levels.begin();
            int64_t level_price = it->first;
            int64_t level_qty   = it->second;
            int64_t fill        = std::min(remaining, level_qty);

            remaining    -= fill;
            total_filled += fill;

            OrderEvent exec{};
            exec.timestamp_ns  = ts_ns;
            exec.order_id      = 0;
            exec.type          = EventType::EXECUTE;
            exec.side          = passive_side;
            exec.price         = level_price;
            exec.qty           = fill;
            exec.qty_remaining = level_qty - fill;
            pending_events_.push_back(make_event(exec, 0));

            // Remove filled live orders at this level
            int64_t to_fill = fill;
            std::vector<uint64_t> to_remove;
            for (auto& [oid, lo] : live_orders_) {
                if (lo.side != passive_side || lo.price != level_price) continue;
                if (to_fill >= lo.qty) {
                    to_fill -= lo.qty;
                    to_remove.push_back(oid);
                } else {
                    lo.qty -= to_fill;
                    to_fill = 0;
                }
                if (to_fill <= 0) break;
            }
            for (uint64_t oid : to_remove) live_orders_.erase(oid);

            it->second -= fill;
            if (it->second <= 0) levels.erase(it);
        }
        return total_filled;
    };

    int64_t filled = (aggressor_side == Side::BID) ? walk_levels(ask_levels_)
                                                    : walk_levels(bid_levels_);
    if (filled > 0) apply_price_impact(aggressor_side, filled);
}

// Δμ = sign × impact_coeff × fill_qty / depth_at_bbo
void DataGenerator::apply_price_impact(Side aggressor_side, int64_t fill_qty) {
    double sign = (aggressor_side == Side::BID) ? 1.0 : -1.0;
    int64_t bbo_depth = 0;
    if (aggressor_side == Side::BID && !ask_levels_.empty())
        bbo_depth = ask_levels_.begin()->second;
    else if (aggressor_side == Side::ASK && !bid_levels_.empty())
        bbo_depth = bid_levels_.begin()->second;

    double avg_depth = std::max(static_cast<double>(cfg_.lot_size),
                                static_cast<double>(bbo_depth));
    mu_ += sign * cfg_.impact_coeff * static_cast<double>(fill_qty) / avg_depth * cfg_.tick_size;
}

GeneratedEvent DataGenerator::make_event(OrderEvent ev, uint64_t wait_ps) const {
    double spread_dollars = spread_ * cfg_.tick_size;
    GeneratedEvent ge{};
    ge.event     = ev;
    ge.mid       = to_fixed(mu_);
    ge.best_bid  = to_fixed(mu_ - spread_dollars / 2.0);
    ge.best_ask  = to_fixed(mu_ + spread_dollars / 2.0);
    ge.depth_bid = bid_levels_.empty() ? 0 : bid_levels_.begin()->second;
    ge.depth_ask = ask_levels_.empty() ? 0 : ask_levels_.begin()->second;
    ge.wait_ps   = wait_ps;
    ge.in_burst  = in_burst_;
    return ge;
}

GeneratedEvent DataGenerator::next() {
    // (a) Drain queued events (EXECUTEs from market matching, ADD from MODIFY)
    if (!pending_events_.empty()) {
        auto ev = pending_events_.front();
        pending_events_.pop_front();
        return ev;
    }

    // (b) Check if earliest lifetime-expiry fires before next Hawkes event
    while (!expiry_queue_.empty()) {
        const auto& top = expiry_queue_.top();
        if (live_orders_.find(top.order_id) == live_orders_.end()) {
            expiry_queue_.pop();  // already cancelled / filled
            continue;
        }
        if (top.expiry_time >= pending_hawkes_time_) break;

        uint64_t oid      = top.order_id;
        double   expiry_t = top.expiry_time;
        expiry_queue_.pop();

        double dt = expiry_t - current_time_;
        step_processes(dt);
        current_time_ = expiry_t;

        auto it = live_orders_.find(oid);
        if (it == live_orders_.end()) continue;

        OrderEvent ev{};
        ev.timestamp_ns  = time_to_ns(current_time_);
        ev.order_id      = oid;
        ev.type          = EventType::CANCEL;
        ev.side          = it->second.side;
        ev.price         = it->second.price;
        ev.qty           = it->second.qty;
        ev.qty_remaining = 0;

        remove_live_order(oid);
        return make_event(ev, apply_jitter(static_cast<uint64_t>(dt * 1e12)));
    }

    // (c) Advance to next Hawkes arrival
    double event_time = pending_hawkes_time_;
    Side   event_side = pending_hawkes_side_;
    double dt         = event_time - current_time_;

    step_processes(dt);
    current_time_ = event_time;

    if (event_side == Side::BID) hawkes_bid_times_.push_back(current_time_);
    else                          hawkes_ask_times_.push_back(current_time_);
    if ((hawkes_bid_times_.size() + hawkes_ask_times_.size()) % 200 == 0)
        prune_hawkes_history();

    update_burst();
    generate_next_hawkes();

    // Sample event type
    double    u         = uniform_(rng_);
    EventType etype;
    bool      is_modify = false;

    if      (u < cfg_.p_add_limit)                              etype = EventType::ADD_LIMIT;
    else if (u < cfg_.p_add_limit + cfg_.p_cancel)              etype = EventType::CANCEL;
    else if (u < cfg_.p_add_limit + cfg_.p_cancel + cfg_.p_add_market) etype = EventType::ADD_MARKET;
    else { etype = EventType::CANCEL; is_modify = true; }

    // No live orders to cancel — fall back to ADD_LIMIT
    if (etype == EventType::CANCEL && live_orders_.empty()) {
        etype     = EventType::ADD_LIMIT;
        is_modify = false;
    }

    uint64_t wait_ps = apply_jitter(static_cast<uint64_t>(dt * 1e12));

    OrderEvent ev{};
    ev.timestamp_ns = time_to_ns(current_time_);

    switch (etype) {
    case EventType::ADD_LIMIT: {
        ev.order_id      = next_order_id_++;
        ev.type          = EventType::ADD_LIMIT;
        ev.side          = event_side;
        ev.price         = sample_limit_price(event_side);
        ev.qty           = sample_qty();
        ev.qty_remaining = ev.qty;

        double lifetime = std::exponential_distribution<double>(cfg_.cancel_rate_gamma)(rng_);
        LiveOrder lo{ev.order_id, ev.side, ev.price, ev.qty, current_time_ + lifetime};
        add_live_order(lo);
        expiry_queue_.push({lo.expiry_time, lo.order_id});
        break;
    }
    case EventType::CANCEL: {
        auto it = live_orders_.begin();
        std::advance(it, std::uniform_int_distribution<size_t>(0, live_orders_.size()-1)(rng_));
        ev.order_id      = it->second.order_id;
        ev.type          = EventType::CANCEL;
        ev.side          = it->second.side;
        ev.price         = it->second.price;
        ev.qty           = it->second.qty;
        ev.qty_remaining = 0;
        remove_live_order(ev.order_id);
        break;
    }
    case EventType::ADD_MARKET: {
        ev.order_id      = next_order_id_++;
        ev.type          = EventType::ADD_MARKET;
        ev.side          = event_side;
        ev.price         = 0;
        ev.qty           = sample_qty();
        ev.qty_remaining = ev.qty;
        match_market_order(event_side, ev.qty, ev.timestamp_ns);
        break;
    }
    default: break;
    }

    auto result = make_event(ev, wait_ps);

    // MODIFY = CANCEL (above) + ADD; queue the ADD half
    if (is_modify) {
        OrderEvent add_ev{};
        add_ev.timestamp_ns  = time_to_ns(current_time_);
        add_ev.order_id      = next_order_id_++;
        add_ev.type          = EventType::ADD_LIMIT;
        add_ev.side          = event_side;
        add_ev.price         = sample_limit_price(event_side);
        add_ev.qty           = sample_qty();
        add_ev.qty_remaining = add_ev.qty;

        double lifetime = std::exponential_distribution<double>(cfg_.cancel_rate_gamma)(rng_);
        LiveOrder lo{add_ev.order_id, add_ev.side, add_ev.price, add_ev.qty,
                     current_time_ + lifetime};
        add_live_order(lo);
        expiry_queue_.push({lo.expiry_time, lo.order_id});
        pending_events_.push_back(make_event(add_ev, 0));
    }

    return result;
}
