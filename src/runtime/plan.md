# Plan: Dual-Thread Engine + ImGui Display Refactor

## Context

The runtime engine currently calls `printf` + `fflush(stdout)` every 64 events from the hot-path polling loop. On high-rate configs (eth_perpetual_futures), this causes tick rate to decay from ~7k/s to ~300/s — terminal I/O is blocking the spin loop. The fix is to move all display off the hot path entirely, publishing a seqlock snapshot that a separate ImGui thread reads at 60fps. Alongside this, the engine is refactored into a clean orchestrator that exposes a zero-overhead user-defined processing interface via C++23 concepts.

---

## Phase 1: Fill in `book_snapshot.hpp`

**File:** `src/runtime/book_snapshot.hpp` (currently empty)

Add a seqlock-protected snapshot struct that the hot path writes and the display thread reads:

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "order_book.hpp"   // PriceLevel

inline constexpr std::size_t SNAPSHOT_DEPTH = 8;

struct alignas(64) BookSnapshot {
    std::atomic<uint64_t> version{0};   // odd = being written; even = stable

    int64_t  best_bid{0};
    int64_t  best_ask{0};
    int64_t  spread{0};
    uint64_t total_bid_qty{0};
    uint64_t total_ask_qty{0};
    double   imbalance{0.0};
    double   vwmid{0.0};              // already divided by PRICE_SCALE
    int64_t  ema{0};
    double   tick_rate{0.0};
    bool     in_burst{false};

    PriceLevel bids[SNAPSHOT_DEPTH]{};
    PriceLevel asks[SNAPSHOT_DEPTH]{};
    uint32_t   bid_level_count{0};
    uint32_t   ask_level_count{0};

    uint64_t event_sequence{0};
};

// Writer helpers (hot-path thread only)
inline void snapshot_begin_write(BookSnapshot& s) noexcept {
    s.version.fetch_add(1, std::memory_order_release);  // -> odd
}
inline void snapshot_end_write(BookSnapshot& s) noexcept {
    s.version.fetch_add(1, std::memory_order_release);  // -> even
}

// Reader (display thread) — returns false if snapshot is unstable or uninitialized
inline bool snapshot_read(const BookSnapshot& src, BookSnapshot& out) noexcept {
    for (int i = 0; i < 64; ++i) {
        uint64_t v1 = src.version.load(std::memory_order_acquire);
        if (v1 & 1u) continue;
        std::memcpy(
            reinterpret_cast<char*>(&out) + sizeof(std::atomic<uint64_t>),
            reinterpret_cast<const char*>(&src) + sizeof(std::atomic<uint64_t>),
            sizeof(BookSnapshot) - sizeof(std::atomic<uint64_t>));
        uint64_t v2 = src.version.load(std::memory_order_acquire);
        if (v1 == v2) return v1 > 0;
    }
    return false;  // contention; caller renders with stale local copy
}
```

The hot path writes between the two `fetch_add` calls using normal stores. The display thread retries on contention but never blocks the engine — it renders the previous frame's data if it can't acquire a stable read.

---

## Phase 2: User Processing Interface (`EventHandler` concept)

**File:** `src/runtime/runtime_engine.hpp` — add before the `RuntimeEngine` class definition

```cpp
#include <concepts>

struct ProcessedEvent {
    uint64_t  sequence;
    uint64_t  timestamp_ns;
    EventType type;
    Side      side;
    int64_t   best_bid;       // fixed-point
    int64_t   best_ask;       // fixed-point
    int64_t   spread;         // fixed-point
    int64_t   ema;            // fixed-point
    double    tick_rate;
    bool      in_burst;
    BurstStats burst_stats;
};

template<typename T>
concept EventHandler = requires(T& h, const ProcessedEvent& e) {
    { h.on_event(e) } noexcept;
};

struct NoOpHandler {
    void on_event(const ProcessedEvent&) noexcept {}
};
```

`ProcessedEvent` intentionally omits depth arrays — walking the book maps per-event is expensive. Depth is only computed at snapshot-publish time (every 64 events). User code needing depth should consume the snapshot directly.

---

## Phase 3: Templatize `RuntimeEngine`

**File:** `src/runtime/runtime_engine.hpp`

```cpp
template<EventHandler Handler = NoOpHandler>
class RuntimeEngine {
public:
    explicit RuntimeEngine(Handler handler = Handler{})
        : handler_(std::move(handler)) {}

    void run(volatile sig_atomic_t& running, BookSnapshot& snapshot);

private:
    ShmManager      shm_;
    TickProcessor   processor_;
    BurstDetector<> detector_;
    OrderBook       book_;
    Handler         handler_;

    uint64_t last_ring_tail_        = 0;
    engine::shm::AcceleratorTick pending_batch_[engine::shm::BATCH_SIZE]{};
    uint32_t pending_count_         = 0;
    uint64_t last_batch_seq_        = 0;
    uint64_t last_result_seq_       = 0;
    uint64_t prev_tick_received_at_ = 0;
    bool     was_in_burst_          = false;

    void flush_batch(engine::shm::SharedMemoryBlock* block, uint64_t seq);
    void publish_snapshot(BookSnapshot& snap, uint64_t seq);
};
```

Because `RuntimeEngine` is now a class template, `run()`, `flush_batch()`, and `publish_snapshot()` must be defined in the header as inline method bodies after the class. The `.cpp` file then retains only the non-template implementations: `TickProcessor` and `BurstDetector`.

**`publish_snapshot()` inline in header:**

```cpp
template<EventHandler H>
void RuntimeEngine<H>::publish_snapshot(BookSnapshot& snap, uint64_t seq) {
    snapshot_begin_write(snap);
    snap.best_bid        = book_.best_bid();
    snap.best_ask        = book_.best_ask();
    snap.spread          = book_.spread();
    snap.total_bid_qty   = book_.total_bid_qty();
    snap.total_ask_qty   = book_.total_ask_qty();
    snap.imbalance       = book_.bid_ask_imbalance(3);
    snap.vwmid           = book_.volume_weighted_mid(3) / PRICE_SCALE;
    snap.ema             = processor_.ema();
    snap.tick_rate       = processor_.tick_rate();
    snap.in_burst        = detector_.in_burst();
    snap.bid_level_count = (uint32_t)book_.bid_depth(snap.bids, SNAPSHOT_DEPTH);
    snap.ask_level_count = (uint32_t)book_.ask_depth(snap.asks, SNAPSHOT_DEPTH);
    snap.event_sequence  = seq;
    snapshot_end_write(snap);
}
```

Note: `volume_weighted_mid()` returns a `double` still in fixed-point units (prices are stored as `int64_t * PRICE_SCALE`). Dividing by `PRICE_SCALE` converts to display units — matching existing `print_book()` behavior.

**Changes to `run()` inner loop:**

- Replace `events_since_print` / `print_book()` with an `events_since_snapshot` counter → call `publish_snapshot(snapshot, slot.sequence)` every 64 events
- Remove all `std::cout` lines from `flush_batch()` and the burst-exit/accelerator-result branches
- After book dispatch + detector/processor updates, build and dispatch `ProcessedEvent`:

```cpp
ProcessedEvent pe{
    .sequence     = slot.sequence,
    .timestamp_ns = slot.timestamp_ns,
    .type         = static_cast<EventType>(slot.type),
    .side         = static_cast<Side>(slot.side),
    .best_bid     = book_.best_bid(),
    .best_ask     = book_.best_ask(),
    .spread       = book_.spread(),
    .ema          = processor_.ema(),
    .tick_rate    = processor_.tick_rate(),
    .in_burst     = burst_now,
    .burst_stats  = detector_.stats(),
};
handler_.on_event(pe);
```

The compiler sees the full concrete type of `Handler` and inlines `on_event` completely — zero runtime dispatch overhead.

---

## Phase 4: `DisplayThread`

**New file:** `src/runtime/display_thread.hpp`

```cpp
#pragma once
#include <csignal>
#include "book_snapshot.hpp"

class DisplayThread {
public:
    void run(const BookSnapshot& snapshot, volatile sig_atomic_t& running);
};
```

**New file:** `src/runtime/display_thread.cpp`

Backend: **GLFW + OpenGL3** — cleanest CMake FetchContent integration, native WSLg support via Mesa.

Render loop outline:
1. `glfwInit()`, create window (900×500, "Order Book"), `glfwMakeContextCurrent`
2. `ImGui::CreateContext()`, `ImGui_ImplGlfw_InitForOpenGL(window, true)`, `ImGui_ImplOpenGL3_Init("#version 330")`
3. `glfwSwapInterval(1)` — vsync caps to ~60fps, no sleep needed
4. Loop until `glfwWindowShouldClose || !running`:
   - `glfwPollEvents()`
   - `snapshot_read(src, local)` — non-blocking; render stale `local` on failure
   - `ImGui_ImplOpenGL3_NewFrame()` / `ImGui_ImplGlfw_NewFrame()` / `ImGui::NewFrame()`
   - `render_order_book(local)`
   - `ImGui::Render()`, `glClear`, `ImGui_ImplOpenGL3_RenderDrawData`, `glfwSwapBuffers`
5. Cleanup: ImGui shutdown → GLFW destroy/terminate

`render_order_book(const BookSnapshot& s)`:
- `ImGui::Begin("Order Book")`
- Header: spread, imbalance, vwmid, EMA, tick rate, burst indicator (highlighted when `in_burst`)
- `ImGui::BeginTable("book", 6)` columns: BID PRICE | QTY | ORD | ORD | QTY | ASK PRICE
- 8 rows; bid cells styled green, ask cells styled red via `ImGui::PushStyleColor`
- `ImGui::End()`

ImGui contexts are not thread-safe — `DisplayThread` must be the only thread calling any `ImGui::` function. The hot-path thread never touches ImGui.

---

## Phase 5: `CMakeLists.txt` Changes

Add after `find_package(Threads REQUIRED)`:

```cmake
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
)
FetchContent_MakeAvailable(imgui)

find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)
```

Update `runtime_engine` target:
```cmake
add_executable(runtime_engine
    src/runtime/main.cpp
    src/runtime/runtime_engine.cpp   # now only TickProcessor/BurstDetector impls
    src/runtime/order_book.cpp
    src/runtime/display_thread.cpp   # NEW
)
target_link_libraries(runtime_engine PRIVATE common Threads::Threads rt imgui_lib)
```

System packages (one-time):
```
sudo apt install libglfw3-dev libgl1-mesa-dev
```

---

## Phase 6: Update `main.cpp`

```cpp
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
```

The main thread becomes the engine (signal delivery is guaranteed on the main thread on Linux). The display thread owns the GLFW window and ImGui context. `snapshot` is the only shared mutable state between threads.

Custom handler — user only modifies `main.cpp`:
```cpp
struct MyStrategy {
    void on_event(const ProcessedEvent& e) noexcept {
        // no I/O, no allocation, no locks
        if (e.in_burst && e.spread < threshold) { /* ... */ }
    }
};
RuntimeEngine<MyStrategy> engine{ MyStrategy{} };
```

---

## File Summary

| File | Action | Key Change |
|------|--------|------------|
| `src/runtime/book_snapshot.hpp` | Fill in (was empty) | `BookSnapshot` seqlock struct + reader/writer helpers |
| `src/runtime/runtime_engine.hpp` | Modify | Add `ProcessedEvent`, `EventHandler` concept, `NoOpHandler`; templatize class; move method bodies inline; remove `print_book` |
| `src/runtime/runtime_engine.cpp` | Modify | Keep only `TickProcessor`/`BurstDetector` impls; delete `print_book`, all `std::cout` |
| `src/runtime/main.cpp` | Modify | Add `BookSnapshot`, launch `DisplayThread` on `std::thread` |
| `src/runtime/display_thread.hpp` | Create | `DisplayThread` class |
| `src/runtime/display_thread.cpp` | Create | GLFW+OpenGL3+ImGui render loop |
| `CMakeLists.txt` | Modify | FetchContent ImGui, glfw3, OpenGL, `imgui_lib` target |

No changes to: `order_book.*`, `shm_types.hpp`, `engine_types.hpp`, `data_sim`, `accelerator_sim`.

---

## Verification

1. `cmake -B build && cmake --build build` — ImGui fetched on first run; clean compile
2. Run `data_sim` then `runtime_engine` → ImGui window opens, order book renders at ~60fps
3. Tick rate shown in GUI should remain stable (~7k/s for eth_perpetual_futures, no decay)
4. SIGINT in either terminal → clean shutdown, window closes, no zombie threads
5. Optionally: add a counter to `NoOpHandler::on_event` in `main.cpp` and print on exit to confirm handler fires for every event
