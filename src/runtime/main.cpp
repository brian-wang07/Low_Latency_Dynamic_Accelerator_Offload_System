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

    DisplayThread display;
    std::thread display_thread([&] {
        display.run(snapshot, g_running);
    });

    RuntimeEngine<> engine;           // NoOpHandler by default
    engine.run(g_running, snapshot);  // main thread = hot path

    display_thread.join();
    return 0;
}
