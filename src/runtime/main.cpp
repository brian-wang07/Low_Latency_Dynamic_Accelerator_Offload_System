#include <csignal>
#include <thread>
#include "runtime_engine.hpp"
#include "display_thread.hpp"
#include "book_snapshot.hpp"

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

int main() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    alignas(64) BookSnapshot snapshot{};

    RuntimeEngine<> engine;
    std::thread engine_thread([&] {
        engine.run(g_running, snapshot);
    });

    DisplayThread display;
    display.run(snapshot, g_running);  // main thread = UI (macOS requirement)

    g_running = 0;
    engine_thread.join();
    return 0;
}
