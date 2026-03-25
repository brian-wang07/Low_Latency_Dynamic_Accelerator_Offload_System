/// Standalone data generator test — runs without shared memory.
///
/// Prints the event stream and accumulates calibration statistics (§6):
///   - spread at BBO
///   - depth at BBO per side
///   - cancel rate (fraction of limit orders that are cancelled vs filled)
///   - market order fraction
///   - event type distribution

#include <iostream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include "../data/config.hpp"
#include "../data/data_generator.hpp"

int main() {
    StreamConfig stream_cfg{};
    DataGeneratorConfig cfg = make_generator_config(stream_cfg);

    DataGenerator generator(cfg);
    using clock = std::chrono::steady_clock;
    auto next_time = clock::now();

    // Calibration counters
    uint64_t n_add_limit  = 0;
    uint64_t n_add_market = 0;
    uint64_t n_cancel     = 0;
    uint64_t n_execute    = 0;
    uint64_t n_total      = 0;
    double   spread_sum   = 0.0;
    double   depth_bid_sum = 0.0;
    double   depth_ask_sum = 0.0;

    constexpr uint64_t N_EVENTS = 100'000;
    constexpr uint64_t PRINT_INTERVAL = 10'000;

    for (uint64_t i = 0; i < N_EVENTS; ++i) {
        auto ge = generator.next();
        auto& ev = ge.event;
        ++n_total;

        switch (ev.type) {
            case EventType::ADD_LIMIT:  ++n_add_limit;  break;
            case EventType::ADD_MARKET: ++n_add_market;  break;
            case EventType::CANCEL:     ++n_cancel;      break;
            case EventType::EXECUTE:    ++n_execute;     break;
            default: break;
        }
        spread_sum    += to_display(ge.best_ask - ge.best_bid);
        depth_bid_sum += ge.depth_bid;
        depth_ask_sum += ge.depth_ask;

        if (i < 50 || i % PRINT_INTERVAL == 0) {
            std::cout << std::fixed << std::setprecision(6)
                << "seq="   << n_total
                << " t="    << event_type_str(ev.type)
                << " s="    << (ev.side == Side::BID ? "B" : "A")
                << " bid="  << to_display(ge.best_bid)
                << " ask="  << to_display(ge.best_ask)
                << " sprd=" << to_display(ge.best_ask - ge.best_bid)
                << " dB="   << ge.depth_bid
                << " dA="   << ge.depth_ask
                << " px="   << (ev.price ? to_display(ev.price) : 0.0)
                << " qty="  << ev.qty
                << " wait=" << ge.wait_ps << "ps"
                << " burst=" << ge.in_burst
                << '\n';
        }
    }

    // Print calibration summary (§6)
    std::cout << "\n── Calibration Summary (" << N_EVENTS << " events) ──\n";
    std::cout << "  ADD_LIMIT:  " << n_add_limit
              << " (" << 100.0 * n_add_limit / n_total << "%)\n";
    std::cout << "  ADD_MARKET: " << n_add_market
              << " (" << 100.0 * n_add_market / n_total << "%)\n";
    std::cout << "  CANCEL:     " << n_cancel
              << " (" << 100.0 * n_cancel / n_total << "%)\n";
    std::cout << "  EXECUTE:    " << n_execute
              << " (" << 100.0 * n_execute / n_total << "%)\n";

    double avg_spread = spread_sum / n_total;
    double avg_depth_bid = depth_bid_sum / n_total;
    double avg_depth_ask = depth_ask_sum / n_total;

    std::cout << "  Avg spread (ticks): " << avg_spread / cfg.tick_size << '\n';
    std::cout << "  Avg depth BID:      " << avg_depth_bid << " shares\n";
    std::cout << "  Avg depth ASK:      " << avg_depth_ask << " shares\n";

    // Cancel rate: cancels / (cancels + executes)
    if (n_cancel + n_execute > 0) {
        std::cout << "  Cancel rate:        "
                  << 100.0 * n_cancel / (n_cancel + n_execute) << "%\n";
    }
    std::cout << "  Market order frac:  "
              << 100.0 * n_add_market / n_total << "%\n";

    // Targets (§6):
    std::cout << "\n── Targets ──\n";
    std::cout << "  Spread at BBO:     1-3 ticks\n";
    std::cout << "  Depth at BBO/side: 200-2000 shares\n";
    std::cout << "  Cancel rate:       >= 90%\n";
    std::cout << "  Market order frac: 8-20%\n";

    return 0;
}
