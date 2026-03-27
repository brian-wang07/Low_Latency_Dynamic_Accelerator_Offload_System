#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

#include "spin_pause.hpp"

#include "../common/shm_manager.hpp"
#include "../common/shm_types.hpp"
#include "config.hpp"
#include "data_generator.hpp"

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

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
        auto last_print = clock::now();
        uint64_t ticks_since_print = 0;

        auto* block = shm.as<engine::shm::SharedMemoryBlock>();
        auto& ring  = block->event_ring;

        // signal runtime to reset book state
        {
            uint64_t h = ring.head.load(std::memory_order_relaxed);
            auto& slot = ring.slots[h & engine::shm::EVENT_RING_MASK];
            slot.sequence = h + 1;
            slot.type = static_cast<uint8_t>(EventType::RESET);
            ring.head.store(h + 1, std::memory_order_release);
        }

        while (g_running) {
            auto ge = generator.next();
            auto& ev = ge.event;

            // spin-wait until ring has space (never drop events)
            uint64_t h = ring.head.load(std::memory_order_relaxed);
            while (h - ring.tail.load(std::memory_order_acquire) >= engine::shm::EVENT_RING_CAPACITY) {
                SPIN_PAUSE();
                if (!g_running) goto done;
            }

            auto& slot         = ring.slots[h & engine::shm::EVENT_RING_MASK];
            slot.sequence      = h + 1;
            slot.timestamp_ns  = ev.timestamp_ns;
            slot.order_id      = ev.order_id;
            slot.price         = ev.price;
            slot.qty           = ev.qty;
            slot.qty_remaining = ev.qty_remaining;
            slot.type          = static_cast<uint8_t>(ev.type);
            slot.side          = static_cast<uint8_t>(ev.side);
            ring.head.store(h + 1, std::memory_order_release);
            ++ticks_since_print;

            auto now_print = clock::now();
            if (now_print - last_print >= std::chrono::seconds(1)) {
                std::cout << std::fixed << std::setprecision(6)
                    << "[" << ticks_since_print << " ticks/s]"
                    << " seq=" << (h + 1)
                    << " t="  << event_type_str(ev.type)
                    << " s="  << (ev.side == Side::BID ? "BID" : "ASK")
                    << " px=" << (ev.price ? to_display(ev.price) : 0.0)
                    << " qty=" << ev.qty
                    << " oid=" << ev.order_id
                    << " burst=" << ge.in_burst
                    << '\n';
                ticks_since_print = 0;
                last_print = now_print;
            }

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
        done:

        std::cout << "\nStopped.\n";
        if (argc > 1) break;
    } while (true);

    return 0;
}
