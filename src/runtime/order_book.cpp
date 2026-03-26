#include "order_book.hpp"

void OrderBook::on_add_limit(uint64_t order_id, uint8_t side, int64_t price, int64_t qty) {
    orders_[order_id] = TrackedOrder {
        .order_id = order_id,
        .side     = side,
        .price    = price,
        .qty      = qty,
    };

    if (side == 0) {
        // BID
        auto &level = bids_[price];
        level.price = price;
        level.total_qty += qty;
        level.order_count++;
    }
    else {
        //ASK
        auto &level = asks_[price];
        level.price = price;
        level.total_qty += qty;
        level.order_count++;
    }
}


void OrderBook::on_execute(uint8_t side, int64_t price, int64_t qty, int64_t qty_remaining) {
    auto do_execute = [&](auto& map) {
        auto lit = map.find(price);
        if (lit == map.end()) return;
        lit->second.total_qty -= qty;
        int64_t to_fill = qty;
        for (auto oit = orders_.begin(); oit != orders_.end() && to_fill > 0; ) {
            TrackedOrder &o = oit->second;
            if (o.side == side && o.price == price) {
                if (o.qty <= to_fill) {
                    to_fill -= o.qty;
                    lit->second.order_count--;
                    oit = orders_.erase(oit);
                } else {
                    o.qty -= to_fill;
                    to_fill = 0;
                }
            } else {
                ++oit;
            }
        }
        if (qty_remaining == 0)
            map.erase(lit);
    };
    if (side == 0) do_execute(bids_);
    else           do_execute(asks_);
}

void OrderBook::on_cancel(uint64_t order_id) {
    if (orders_.find(order_id) == orders_.end())
        return;

    TrackedOrder order = orders_[order_id];
    uint8_t side = order.side;

    PriceLevel &level = (side == 0) ? bids_[order.price] : asks_[order.price];
    level.total_qty -= order.qty;
    level.order_count--;

    if (level.total_qty == 0) {
        (side == 0) ? bids_.erase(order.price) : asks_.erase(order.price);
    }
    orders_.erase(order_id);
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    orders_.clear();
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




