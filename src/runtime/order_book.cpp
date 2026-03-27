#include "order_book.hpp"

void OrderBook::on_add_limit(uint64_t order_id, uint8_t side, int64_t price, int64_t qty) {
    orders_[order_id] = TrackedOrder {
        .order_id = order_id,
        .side     = side,
        .price    = price,
        .qty      = qty,
    };

    if (side == 0) {
        auto &level = bids_[price];
        level.price = price;
        level.total_qty += qty;
        level.order_count++;
        bid_order_idx_[price].push_back(order_id);
    }
    else {
        auto &level = asks_[price];
        level.price = price;
        level.total_qty += qty;
        level.order_count++;
        ask_order_idx_[price].push_back(order_id);
    }
}


void OrderBook::on_execute(uint8_t side, int64_t price, int64_t qty, int64_t qty_remaining) {
    auto do_execute = [&](auto& level_map, auto& idx_map) {
        auto lit = level_map.find(price);
        if (lit == level_map.end()) return;
        lit->second.total_qty -= qty;

        int64_t to_fill = qty;
        auto idx_it = idx_map.find(price);
        if (idx_it != idx_map.end()) {
            auto& ids = idx_it->second;
            std::size_t i = 0;
            while (i < ids.size() && to_fill > 0) {
                auto oit = orders_.find(ids[i]);
                if (oit == orders_.end()) {
                    ids[i] = ids.back();
                    ids.pop_back();
                    continue;
                }
                TrackedOrder& o = oit->second;
                if (o.qty <= to_fill) {
                    to_fill -= o.qty;
                    lit->second.order_count--;
                    orders_.erase(oit);
                    ids[i] = ids.back();
                    ids.pop_back();
                } else {
                    o.qty -= to_fill;
                    to_fill = 0;
                    ++i;
                }
            }
            if (ids.empty()) idx_map.erase(idx_it);
        }

        if (qty_remaining == 0)
            level_map.erase(lit);
    };
    if (side == 0) do_execute(bids_, bid_order_idx_);
    else           do_execute(asks_, ask_order_idx_);
}

void OrderBook::on_cancel(uint64_t order_id) {
    auto oit = orders_.find(order_id);
    if (oit == orders_.end()) return;

    TrackedOrder order = oit->second;
    uint8_t side = order.side;

    auto erase_from_level = [&](auto& level_map) {
        auto lit = level_map.find(order.price);
        if (lit != level_map.end()) {
            lit->second.total_qty -= order.qty;
            lit->second.order_count--;
            if (lit->second.total_qty <= 0)
                level_map.erase(lit);
        }
    };

    if (side == 0) erase_from_level(bids_);
    else           erase_from_level(asks_);

    auto& idx_map = (side == 0) ? bid_order_idx_ : ask_order_idx_;
    auto idx_it = idx_map.find(order.price);
    if (idx_it != idx_map.end()) {
        auto& ids = idx_it->second;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == order_id) {
                ids[i] = ids.back();
                ids.pop_back();
                break;
            }
        }
        if (ids.empty()) idx_map.erase(idx_it);
    }

    orders_.erase(oit);
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    orders_.clear();
    bid_order_idx_.clear();
    ask_order_idx_.clear();
}



int64_t OrderBook::best_bid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

int64_t OrderBook::best_ask() const {
    return asks_.empty() ? 0 : asks_.begin()->first;
}

int64_t OrderBook::spread() const {
    if (bids_.empty() || asks_.empty()) return 0;
    return best_ask() - best_bid();
}

double OrderBook::bid_ask_imbalance(std::size_t levels) const {
    int64_t bid_vol = 0, ask_vol = 0;
    std::size_t i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < levels; ++it, ++i)
        bid_vol += it->second.total_qty;
    i = 0;
    for (auto it = asks_.begin(); it != asks_.end() && i < levels; ++it, ++i)
        ask_vol += it->second.total_qty;
    int64_t total = bid_vol + ask_vol;
    if (total == 0) return 0.0;
    return static_cast<double>(bid_vol - ask_vol) / static_cast<double>(total);
}

double OrderBook::volume_weighted_mid(std::size_t levels) const {
    __int128 total_value = 0;
    int64_t  total_qty   = 0;
    std::size_t i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < levels; ++it, ++i) {
        total_value += static_cast<__int128>(it->first) * it->second.total_qty;
        total_qty   += it->second.total_qty;
    }
    i = 0;
    for (auto it = asks_.begin(); it != asks_.end() && i < levels; ++it, ++i) {
        total_value += static_cast<__int128>(it->first) * it->second.total_qty;
        total_qty   += it->second.total_qty;
    }
    if (total_qty == 0) return 0.0;
    return static_cast<double>(total_value) / static_cast<double>(total_qty);
}

std::size_t OrderBook::bid_depth(PriceLevel *out, std::size_t max_levels) const {
    std::size_t i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < max_levels; ++it, ++i)
        out[i] = it->second;
    return i;
}

std::size_t OrderBook::ask_depth(PriceLevel *out, std::size_t max_levels) const {
    std::size_t i = 0;
    for (auto it = asks_.begin(); it != asks_.end() && i < max_levels; ++it, ++i)
        out[i] = it->second;
    return i;
}

uint64_t OrderBook::total_bid_qty() const {
    uint64_t sum = 0;
    for (const auto& [price, level] : bids_)
        sum += level.total_qty;
    return sum;
}

uint64_t OrderBook::total_ask_qty() const {
    uint64_t sum = 0;
    for (const auto& [price, level] : asks_)
        sum += level.total_qty;
    return sum;
}




