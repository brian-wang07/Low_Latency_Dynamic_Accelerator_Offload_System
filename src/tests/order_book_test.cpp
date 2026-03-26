#include "order_book.hpp"
#include "engine_types.hpp"
#include <cstdio>
#include <cstdint>

static int64_t px(double p) { return static_cast<int64_t>(p * PRICE_SCALE); }

static void print_book(const OrderBook& book) {
    constexpr std::size_t DEPTH = 8;
    PriceLevel bids[DEPTH], asks[DEPTH];
    std::size_t nb = book.bid_depth(bids, DEPTH);
    std::size_t na = book.ask_depth(asks, DEPTH);

    printf("\n");
    printf("  %-12s %-10s %-6s   %-6s %-10s %-12s\n",
           "BID PRICE", "QTY", "COUNT", "COUNT", "QTY", "ASK PRICE");
    printf("  %-12s %-10s %-6s   %-6s %-10s %-12s\n",
           "---------", "---", "-----", "-----", "---", "---------");

    std::size_t rows = nb > na ? nb : na;
    for (std::size_t i = 0; i < rows; ++i) {
        // bid side
        if (i < nb)
            printf("  %-12.6f %-10ld %-6u   ",
                   static_cast<double>(bids[i].price) / PRICE_SCALE,
                   bids[i].total_qty,
                   bids[i].order_count);
        else
            printf("  %-12s %-10s %-6s   ", "", "", "");

        // ask side
        if (i < na)
            printf("%-6u %-10ld %-12.6f\n",
                   asks[i].order_count,
                   asks[i].total_qty,
                   static_cast<double>(asks[i].price) / PRICE_SCALE);
        else
            printf("\n");
    }

    printf("\n");
    printf("  best bid: %.6f  best ask: %.6f  spread: %.6f\n",
           static_cast<double>(book.best_bid()) / PRICE_SCALE,
           static_cast<double>(book.best_ask()) / PRICE_SCALE,
           static_cast<double>(book.spread())   / PRICE_SCALE);
    printf("  imbalance: %.4f  vwmid: %.6f\n",
           book.bid_ask_imbalance(3),
           book.volume_weighted_mid(3) / PRICE_SCALE);
    printf("  total bid qty: %lu  total ask qty: %lu\n",
           book.total_bid_qty(), book.total_ask_qty());
    printf("\n");
}

// ---- helpers ----------------------------------------------------------------

static int passed = 0, failed = 0;

#define CHECK(cond) do { \
    if (cond) { printf("  [PASS] %s\n", #cond); ++passed; } \
    else      { printf("  [FAIL] %s  (line %d)\n", #cond, __LINE__); ++failed; } \
} while(0)

// ---- tests ------------------------------------------------------------------

static void test_add_and_bbo() {
    printf("=== test_add_and_bbo ===\n");
    OrderBook book;
    book.on_add_limit(1, 0, px(99.5), 100);   // bid
    book.on_add_limit(2, 0, px(99.0), 200);   // bid lower
    book.on_add_limit(3, 1, px(100.0), 150);  // ask
    book.on_add_limit(4, 1, px(100.5), 50);   // ask higher

    CHECK(book.best_bid() == px(99.5));
    CHECK(book.best_ask() == px(100.0));
    CHECK(book.spread()   == px(0.5));
    print_book(book);
}

static void test_cancel() {
    printf("=== test_cancel ===\n");
    OrderBook book;
    book.on_add_limit(1, 0, px(99.5), 100);
    book.on_add_limit(2, 0, px(99.5), 50);   // same level, second order
    book.on_add_limit(3, 1, px(100.0), 200);

    book.on_cancel(1);  // removes 100 from 99.5 level
    CHECK(book.best_bid() == px(99.5));       // level still exists via order 2

    PriceLevel bids[4];
    std::size_t nb = book.bid_depth(bids, 4);
    CHECK(nb == 1);
    CHECK(bids[0].total_qty == 50);
    CHECK(bids[0].order_count == 1);

    book.on_cancel(2);  // level now empty — should be erased
    CHECK(book.best_bid() == 0);
    print_book(book);
}

static void test_cancel_unknown_is_noop() {
    printf("=== test_cancel_unknown_is_noop ===\n");
    OrderBook book;
    book.on_add_limit(1, 0, px(99.0), 100);
    book.on_cancel(999);  // unknown — must not crash or corrupt
    CHECK(book.best_bid() == px(99.0));
    CHECK(book.total_bid_qty() == 100);
    print_book(book);
}

static void test_execute_partial() {
    printf("=== test_execute_partial ===\n");
    OrderBook book;
    book.on_add_limit(1, 1, px(100.0), 300);
    book.on_add_limit(2, 1, px(100.0), 200);
    book.on_add_limit(3, 0, px(99.0),  500);

    // partial fill: 200 filled, 300 remaining
    book.on_execute(1, px(100.0), 200, 300);

    PriceLevel asks[4];
    std::size_t na = book.ask_depth(asks, 4);
    CHECK(na == 1);
    CHECK(asks[0].total_qty == 300);
    print_book(book);
}

static void test_execute_full_erases_level() {
    printf("=== test_execute_full_erases_level ===\n");
    OrderBook book;
    book.on_add_limit(1, 1, px(100.0), 100);
    book.on_add_limit(2, 0, px(99.0),  100);

    book.on_execute(1, px(100.0), 100, 0);  // qty_remaining == 0 → erase level

    CHECK(book.best_ask() == 0);
    print_book(book);
}

static void test_bid_ask_invariant() {
    printf("=== test_bid_ask_invariant ===\n");
    OrderBook book;
    book.on_add_limit(1, 0, px(98.0),  100);
    book.on_add_limit(2, 0, px(99.0),  200);
    book.on_add_limit(3, 1, px(101.0), 150);
    book.on_add_limit(4, 1, px(102.0), 100);

    CHECK(book.best_bid() < book.best_ask());
    CHECK(book.spread() > 0);
    CHECK(book.total_bid_qty() == 300);
    CHECK(book.total_ask_qty() == 250);
    print_book(book);
}

// -----------------------------------------------------------------------------

int main() {
    test_add_and_bbo();
    test_cancel();
    test_cancel_unknown_is_noop();
    test_execute_partial();
    test_execute_full_erases_level();
    test_bid_ask_invariant();

    printf("=== results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
