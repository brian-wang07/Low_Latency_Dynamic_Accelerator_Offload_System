#pragma once

#include <algorithm>
#include <concepts>
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <chrono>
#include "spin_pause.hpp"

#include "shm_manager.hpp"
#include "shm_types.hpp"
#include "order_book.hpp"
#include "book_snapshot.hpp"
#include "engine_types.hpp"

struct LatencyTracker {
    // Collect raw samples per 64-event snapshot window.
    // Sorting 64 elements in publish_snapshot is trivial; no heap allocation needed.
    static constexpr uint32_t MAX_SAMPLES = 64;
    uint64_t samples[MAX_SAMPLES]{};
    uint32_t count = 0;

    // O(1) — hot event path
    void record(uint64_t latency_ns) noexcept {
        if (count < MAX_SAMPLES)
            samples[count++] = latency_ns;
    }

    // O(1) — just reset the count
    void reset() noexcept { count = 0; }

    // O(N log N) on 64 elements — negligible; called from publish_snapshot only
    std::pair<double,double> p50_p99_us() const noexcept {
        if (count == 0) return {0.0, 0.0};
        uint64_t sorted[MAX_SAMPLES];
        std::copy(samples, samples + count, sorted);
        std::sort(sorted, sorted + count);
        double p50 = sorted[count / 2] / 1000.0;
        double p99 = sorted[count * 99u / 100u] / 1000.0;
        return {p50, p99};
    }
};

struct BurstStats {
    uint64_t mean_inter_arrival_ns;
    uint64_t burst_entry_ns;
    uint64_t burst_tick_count;
};


struct BurstCfg {
    uint64_t burst_threshold_ns;
    uint64_t burst_exit_threshold_ns;
};

class TickProcessor {
public:
    explicit TickProcessor(double alpha = 0.05);
    void on_tick(int64_t mid, uint64_t received_at_ns);
    int64_t ema() const noexcept { return ema_; }
    double  tick_rate() const noexcept { return tick_rate_; }

private:
    double   alpha_;
    int64_t  ema_ = 0;
    bool     first_ = true;
    uint64_t window_start_ns_ = 0;
    int      ticks_in_window_ = 0;
    double   tick_rate_ = 0.0;
};

template<std::size_t WindowSize = 16>
class BurstDetector {
public:
    explicit BurstDetector(BurstCfg cfg);
    BurstDetector();

    void on_tick(uint64_t received_at_ns);
    bool in_burst() const noexcept;
    BurstStats stats() const noexcept;
    double threshold_tps() const noexcept {
        return burst_threshold_ns_ > 0 ? 1e9 / double(burst_threshold_ns_) : 0.0;
    }

private:
    std::array<uint64_t, WindowSize> timestamps_{};
    bool        in_burst_ = false;
    std::size_t head_   = 0;
    std::size_t filled_ = 0;
    uint64_t    burst_threshold_ns_;
    uint64_t    burst_exit_threshold_ns_;
    uint64_t    burst_entry_ns_    = 0;
    uint64_t    burst_tick_count_  = 0;
    uint64_t    running_sum_       = 0;
};

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

//EventHandler generic requires on_event method with noexcept
template <typename T>
concept EventHandler = requires (T& h, const ProcessedEvent &e) {
    { h.on_event(e) } noexcept;
};

struct NoOpHandler {
    void on_event(const ProcessedEvent&) noexcept {}   
};


template<EventHandler Handler = NoOpHandler>
class RuntimeEngine {
public:
    explicit RuntimeEngine(Handler handler = Handler{}) :
        handler_(std::move(handler)) {}
    void run(volatile sig_atomic_t& running, BookSnapshot& snapshot);

private:
    static double calibrate_tsc_ns_per_cycle() noexcept {
        using clock = std::chrono::steady_clock;
        (void)__rdtsc();  // warm up
        auto wall_start   = clock::now();
        uint64_t tsc_start = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint64_t tsc_end  = __rdtsc();
        auto wall_end     = clock::now();
        double elapsed_ns  = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 wall_end - wall_start).count();
        return elapsed_ns / (double)(tsc_end - tsc_start);
    }

    ShmManager shm_;
    TickProcessor processor_;
    Handler handler_;
    BurstDetector<> detector_;
    OrderBook book_;
    LatencyTracker lat_tracker_;
    double tsc_to_ns_ = 1.0;

    // ring consumer state
    uint64_t last_ring_tail_ = 0;

    // accelerator batch accumulation
    engine::shm::AcceleratorTick pending_batch_[engine::shm::BATCH_SIZE]{};
    uint32_t pending_count_         = 0;
    uint64_t last_batch_seq_        = 0;
    uint64_t last_result_seq_       = 0;
    uint64_t prev_tick_received_at_ = 0;
    bool     was_in_burst_          = false;

    void flush_batch(engine::shm::SharedMemoryBlock* block, uint64_t seq);
    void publish_snapshot(BookSnapshot &snap, uint64_t seq); 
};


template<EventHandler H>
void RuntimeEngine<H>::publish_snapshot(BookSnapshot& snap, uint64_t seq) {
    auto [p50, p99] = lat_tracker_.p50_p99_us();
    lat_tracker_.reset();

    snapshot_begin_write(snap);
    snap.best_bid             = book_.best_bid();
    snap.best_ask             = book_.best_ask();
    snap.spread               = book_.spread();
    snap.total_bid_qty        = book_.total_bid_qty();
    snap.total_ask_qty        = book_.total_ask_qty();
    snap.imbalance            = book_.bid_ask_imbalance(3);
    snap.vwmid                = book_.volume_weighted_mid(3) / PRICE_SCALE;
    snap.ema                  = processor_.ema();
    snap.tick_rate            = processor_.tick_rate();
    snap.in_burst             = detector_.in_burst();
    snap.bid_level_count      = (uint32_t)book_.bid_depth(snap.bids, SNAPSHOT_DEPTH);
    snap.ask_level_count      = (uint32_t)book_.ask_depth(snap.asks, SNAPSHOT_DEPTH);
    snap.event_sequence       = seq;
    snap.latency_p50_us       = p50;
    snap.latency_p99_us       = p99;
    snap.ring_occupancy       = double(shm_.as<engine::shm::SharedMemoryBlock>()->event_ring.head.load(std::memory_order_relaxed)
                                     - shm_.as<engine::shm::SharedMemoryBlock>()->event_ring.tail.load(std::memory_order_relaxed))
                                / double(engine::shm::EVENT_RING_CAPACITY);
    snap.burst_threshold_tps  = detector_.threshold_tps();
    snapshot_end_write(snap);
}


template <EventHandler H>
void RuntimeEngine<H>::flush_batch(engine::shm::SharedMemoryBlock* block, uint64_t seq) {
    auto& batch = block->data_to_accelerator;
    batch.burst_meta.tick_count = pending_count_;
    for (uint32_t i = 0; i < pending_count_; ++i)
        batch.ticks[i] = pending_batch_[i];
    batch.count = pending_count_;
    batch.batch_sequence_number.store(++last_batch_seq_, std::memory_order_release);
    block->accelerator_signal.routing_active.store(true, std::memory_order_release);

    auto s = detector_.stats();
    pending_count_ = 0;
}

template <EventHandler H>
void RuntimeEngine<H>::run(volatile sig_atomic_t& running, BookSnapshot& snapshot) {
    std::cout << "Calibrating TSC...\n";
    tsc_to_ns_ = calibrate_tsc_ns_per_cycle();
    std::cout << "TSC: " << tsc_to_ns_ << " ns/cycle\n";

    std::cout << "Waiting for shared memory...\n";
    while (!shm_.open()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Attached to shared memory. Polling...\n";

    auto* block = shm_.as<engine::shm::SharedMemoryBlock>();
    auto& ring  = block->event_ring;
    uint64_t events_since_snapshot = 0;
    uint64_t last_seq = 0;

    while (running) {
        uint64_t head = ring.head.load(std::memory_order_acquire);
        if (head == last_ring_tail_) {
            SPIN_PAUSE();
            continue;
        }

        while (last_ring_tail_ < head) {
            const auto& slot = ring.slots[last_ring_tail_ & engine::shm::EVENT_RING_MASK];

            // book dispatch
            switch (slot.type) {
                case static_cast<uint8_t>(EventType::ADD_LIMIT):
                    book_.on_add_limit(slot.order_id, slot.side, slot.price, slot.qty);
                    break;
                case static_cast<uint8_t>(EventType::CANCEL):
                    book_.on_cancel(slot.order_id);
                    break;
                case static_cast<uint8_t>(EventType::EXECUTE):
                    book_.on_execute(slot.side, slot.price, slot.qty, slot.qty_remaining);
                    break;
                case static_cast<uint8_t>(EventType::RESET):
                    book_.clear();
                    break;
                default:
                    break;
            }

            // derive market state from book
            int64_t bid = book_.best_bid();
            int64_t ask = book_.best_ask();
            int64_t mid = (bid && ask) ? (bid + ask) / 2 : (bid | ask);
            uint64_t received_tsc = __rdtsc();
            uint64_t received_at  = (uint64_t)(received_tsc * tsc_to_ns_);

            detector_.on_tick(slot.timestamp_ns);
            processor_.on_tick(mid, received_at);
            if (slot.enqueue_tsc > 0)
                lat_tracker_.record((uint64_t)((received_tsc - slot.enqueue_tsc) * tsc_to_ns_));

            // burst / accelerator routing
            bool burst_now = detector_.in_burst();

            if (burst_now) {
                uint64_t delta = (prev_tick_received_at_ > 0)
                    ? received_at - prev_tick_received_at_ : 0;

                if (pending_count_ < engine::shm::BATCH_SIZE) {
                    pending_batch_[pending_count_++] = engine::shm::AcceleratorTick{
                        .sequence_number  = slot.sequence,
                        .price            = mid,
                        .arrival_delta_ns = delta,
                    };
                }

                if (!was_in_burst_) {
                    auto& batch = block->data_to_accelerator;
                    batch.burst_meta.burst_entry_time_ns = received_at;
                    batch.burst_meta.ema_at_entry        = processor_.ema();
                }

                if (pending_count_ == engine::shm::BATCH_SIZE)
                    flush_batch(block, slot.sequence);
            } else {
                if (was_in_burst_) {
                    if (pending_count_ > 0)
                        flush_batch(block, slot.sequence);
                }

                uint64_t result_seq = block->accelerator_signal.result_sequence_number
                                          .load(std::memory_order_acquire);
                if (result_seq > last_result_seq_) {
                    last_result_seq_ = result_seq;
                    int64_t proc_ema = block->accelerator_signal.processed_ema.load(std::memory_order_relaxed);
                    int8_t  action   = block->accelerator_signal.signal_action.load(std::memory_order_relaxed);
                    std::cout << "[ACCEL] result_seq=" << result_seq
                              << " processed_ema=" << to_display(proc_ema)
                              << " action=" << static_cast<int>(action) << '\n';
                }
            }

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

            was_in_burst_          = burst_now;
            prev_tick_received_at_ = received_at;
            last_seq               = slot.sequence;
            ++last_ring_tail_;
            ++events_since_snapshot;
        }

        ring.tail.store(last_ring_tail_, std::memory_order_release);

        if (events_since_snapshot >= 64) {
            publish_snapshot(snapshot, last_seq);
            events_since_snapshot = 0;
        }

    }
}
