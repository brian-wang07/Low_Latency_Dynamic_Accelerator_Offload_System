#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

struct PriceLevel {
    int64_t  price;
    int64_t  total_qty;
    uint32_t order_count;
};

struct TrackedOrder {
    uint64_t order_id;
    uint8_t  side;
    int64_t  price;
    int64_t  qty;
};

class OrderBook {
public:
    void on_add_limit(uint64_t order_id, uint8_t side, int64_t price, int64_t qty);
    void on_cancel(uint64_t order_id);
    void on_execute(uint8_t side, int64_t price, int64_t qty, int64_t qty_remaining);
    void clear();

    // queries
    int64_t best_bid() const;
    int64_t best_ask() const;
    int64_t spread() const;
    double  bid_ask_imbalance(std::size_t levels = 1) const;
    double  volume_weighted_mid(std::size_t levels = 3) const;

    // depth - return up to max_levels
    std::size_t bid_depth(PriceLevel *out, std::size_t max_levels) const;
    std::size_t ask_depth(PriceLevel *out, std::size_t max_levels) const;

    uint64_t total_bid_qty() const;
    uint64_t total_ask_qty() const;

private:
    std::map<int64_t, PriceLevel, std::greater<>> bids_; //descending order; begin() = best bid (highest)
    std::map<int64_t, PriceLevel>                 asks_; //ascending order; begin() = best ask (lowest)
    std::unordered_map<uint64_t, TrackedOrder>    orders_;

    // price -> [order_ids] index for O(1) lookup in on_execute
    std::unordered_map<int64_t, std::vector<uint64_t>> bid_order_idx_;
    std::unordered_map<int64_t, std::vector<uint64_t>> ask_order_idx_;
};

