#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

#include "../common/shm_manager.hpp"
#include "../common/shm_types.hpp"
#include "config.hpp"
#include "data_generator.hpp"

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

static const char* side_str(Side s) {
    return (s == Side::BID) ? "BID" : "ASK";
}

int main(int argc, char* argv[]) {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    namespace fs = std::filesystem;
    const fs::path configs_dir = fs::path(PROJECT_ROOT) / "configs";

    ShmManager shm;
    if (!shm.create()) {
        std::cerr << "Failed to create shared memory\n";
        return 1;
    }

    do {
        g_running = 1;

        fs::path cfg_path;

        if (argc > 1) {
            // Direct path from command line — run once, no loop
            cfg_path = argv[1];
        } else {
            // Interactive picker from configs/
            std::vector<std::string> configs;
            if (fs::exists(configs_dir)) {
                for (auto& e : fs::directory_iterator(configs_dir))
                    if (e.path().extension() == ".json")
                        configs.push_back(e.path().filename().string());
                std::sort(configs.begin(), configs.end());
            }

            if (configs.empty()) {
                std::cerr << "No .json files found in " << configs_dir << '\n';
                break;
            }

            std::cout << "\nAvailable configs:\n";
            for (size_t i = 0; i < configs.size(); ++i)
                std::cout << "  " << (i + 1) << ") " << configs[i] << '\n';
            std::cout << "Select (1-" << configs.size() << ", or q to quit): ";

            std::string input;
            if (!std::getline(std::cin, input) || input == "q" || input == "Q") break;

            size_t idx;
            try   { idx = std::stoul(input) - 1; }
            catch (...) { std::cerr << "Invalid selection.\n"; continue; }
            if (idx >= configs.size()) { std::cerr << "Out of range.\n"; continue; }

            cfg_path = configs_dir / configs[idx];
        }

        DataGeneratorConfig cfg;
        try   { cfg = load_config(cfg_path.string()); }
        catch (const std::exception& e) {
            std::cerr << "Error loading config: " << e.what() << '\n';
            if (argc > 1) return 1;
            continue;
        }

        std::cout << "Running " << cfg_path.filename() << " (Ctrl+C to stop)...\n";

        DataGenerator generator(cfg);
        using clock = std::chrono::steady_clock;
        auto next_time = clock::now();
        uint64_t shm_seq = 0;

        while (g_running) {
            auto ge = generator.next();
            auto& ev = ge.event;

            auto* block = shm.as<engine::shm::SharedMemoryBlock>();
            auto& md    = block->latest_market_data;

            md.bid.store       (ge.best_bid,                                              std::memory_order_relaxed);
            md.ask.store       (ge.best_ask,                                              std::memory_order_relaxed);
            md.depth.store     (static_cast<uint32_t>(ge.depth_bid + ge.depth_ask),       std::memory_order_relaxed);
            md.price.store     (ge.mid,                                                   std::memory_order_relaxed);
            md.order_type.store(static_cast<uint8_t>(ev.type),                            std::memory_order_relaxed);
            md.side.store      (static_cast<uint8_t>(ev.side),                            std::memory_order_relaxed);
            md.action.store    (static_cast<uint8_t>(ev.type),                            std::memory_order_relaxed);
            md.timestamp.store (std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    clock::now().time_since_epoch()).count(),              std::memory_order_relaxed);
            md.sequence_number.store(++shm_seq,                                           std::memory_order_release);

            std::cout << std::fixed << std::setprecision(6)
                << "seq="   << shm_seq
                << " t="    << event_type_str(ev.type)
                << " s="    << side_str(ev.side)
                << " bid="  << to_display(ge.best_bid)
                << " ask="  << to_display(ge.best_ask)
                << " sprd=" << to_display(ge.best_ask - ge.best_bid)
                << " dB="   << ge.depth_bid
                << " dA="   << ge.depth_ask
                << " px="   << (ev.price ? to_display(ev.price) : 0.0)
                << " qty="  << ev.qty
                << " oid="  << ev.order_id
                << " wait=" << (ge.wait_ps / 1000.0) << "ns"
                << " burst=" << ge.in_burst
                << '\n';

            if (ge.wait_ps > 0) {
                next_time += std::chrono::nanoseconds(ge.wait_ps / 1000);

                while (g_running) {
                    auto now = clock::now();
                    if (now >= next_time) break;
                    auto remaining = next_time - now;
                    if (remaining > std::chrono::microseconds(50))
                        std::this_thread::sleep_for(remaining - std::chrono::microseconds(20));
                }
            }
        }

        std::cout << "\nStopped.\n";
        if (argc > 1) break;
    } while (true);

    return 0;
}
