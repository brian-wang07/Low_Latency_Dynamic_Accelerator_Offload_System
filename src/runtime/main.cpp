#include <csignal>
#include "runtime_engine.hpp"

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

int main() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    RuntimeEngine engine;
    engine.run(g_running);
    return 0;
}
