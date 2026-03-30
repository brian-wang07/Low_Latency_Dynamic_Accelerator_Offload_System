#pragma once

#include <cstdint>

struct PriceLevel {
    int64_t  price;
    int64_t  total_qty;
    uint32_t order_count;
};
