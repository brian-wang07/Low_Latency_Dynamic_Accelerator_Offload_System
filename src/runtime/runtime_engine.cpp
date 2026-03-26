#include "runtime_engine.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

#include <immintrin.h>

#include "engine_types.hpp"


template<std::size_t WindowSize>
BurstDetector<WindowSize>::BurstDetector(BurstCfg cfg)
    : burst_threshold_ns_(cfg.burst_threshold_ns),
      burst_exit_threshold_ns_(cfg.burst_exit_threshold_ns) {}

template<std::size_t WindowSize>
BurstDetector<WindowSize>::BurstDetector()
    : BurstDetector(BurstCfg{
        .burst_threshold_ns      = 20,
        .burst_exit_threshold_ns = 35,
      }) {}

template<std::size_t WindowSize>
void BurstDetector<WindowSize>::on_tick(uint64_t received_at_ns) {
    if (filled_ == 0) {
        timestamps_[head_] = received_at_ns;
        head_   = (head_ + 1) % WindowSize;
        filled_ = 1;
        return;
    }

    uint64_t prev      = timestamps_[(head_ + WindowSize - 1) % WindowSize];
    uint64_t new_delta = received_at_ns - prev;

    if (filled_ == WindowSize) {
        uint64_t evicted = timestamps_[(head_ + 1) % WindowSize] - timestamps_[head_];
        running_sum_ -= evicted;
    } else {
        ++filled_;
    }

    running_sum_ += new_delta;
    timestamps_[head_] = received_at_ns;
    head_ = (head_ + 1) % WindowSize;

    uint64_t mean = running_sum_ / (filled_ - 1);

    if (!in_burst_ && mean < burst_threshold_ns_) {
        in_burst_         = true;
        burst_entry_ns_   = received_at_ns;
        burst_tick_count_ = 1;
    } else if (in_burst_) {
        ++burst_tick_count_;
        if (mean > burst_exit_threshold_ns_)
            in_burst_ = false;
    }
}

template<std::size_t WindowSize>
bool BurstDetector<WindowSize>::in_burst() const noexcept {
    return in_burst_;
}

template<std::size_t WindowSize>
BurstStats BurstDetector<WindowSize>::stats() const noexcept {
    uint64_t mean = (filled_ > 1) ? running_sum_ / (filled_ - 1) : 0;
    return BurstStats{
        .mean_inter_arrival_ns = mean,
        .burst_entry_ns        = burst_entry_ns_,
        .burst_tick_count      = burst_tick_count_,
    };
}

template class BurstDetector<16>;


TickProcessor::TickProcessor(double alpha) : alpha_(alpha) {}

void TickProcessor::on_tick(int64_t mid, uint64_t received_at_ns) {
    if (first_) {
        ema_ = mid;
        first_ = false;
        window_start_ns_ = received_at_ns;
    } else {
        ema_ = static_cast<int64_t>(alpha_ * mid + (1.0 - alpha_) * ema_);
    }

    ++ticks_in_window_;

    uint64_t elapsed_ns = received_at_ns - window_start_ns_;
    if (elapsed_ns >= 1'000'000'000ULL) {
        tick_rate_ = static_cast<double>(ticks_in_window_) * 1e9 / static_cast<double>(elapsed_ns);
        ticks_in_window_ = 0;
        window_start_ns_ = received_at_ns;
    }
}


void RuntimeEngine::flush_batch(engine::shm::SharedMemoryBlock* block, uint64_t seq) {
    auto& batch = block->data_to_accelerator;
    batch.burst_meta.tick_count = pending_count_;
    for (uint32_t i = 0; i < pending_count_; ++i)
        batch.ticks[i] = pending_batch_[i];
    batch.count = pending_count_;
    batch.batch_sequence_number.store(++last_batch_seq_, std::memory_order_release);
    block->accelerator_signal.routing_active.store(true, std::memory_order_release);

    auto s = detector_.stats();
    std::cout << "[ROUTE] seq=" << seq
              << " batch_size=" << pending_count_
              << " burst_ticks=" << s.burst_tick_count
              << " ema_at_entry=" << to_display(batch.burst_meta.ema_at_entry) << '\n';

    pending_count_ = 0;
}

void RuntimeEngine::run(volatile sig_atomic_t& running) {
    std::cout << "Waiting for shared memory...\n";
    while (!shm_.open()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Attached to shared memory. Polling...\n";

    auto* block = shm_.as<engine::shm::SharedMemoryBlock>();
    auto& ring  = block->event_ring;
    uint64_t events_since_print = 0;

    while (running) {
        uint64_t head = ring.head.load(std::memory_order_acquire);
        if (head == last_ring_tail_) {
            _mm_pause();
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
            uint64_t received_at = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            detector_.on_tick(slot.timestamp_ns);
            processor_.on_tick(mid, received_at);

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

                    auto s = detector_.stats();
                    std::cout << "[ROUTE END] burst_ticks=" << s.burst_tick_count
                              << " mean_inter_arrival_ns=" << s.mean_inter_arrival_ns << '\n';
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

            was_in_burst_          = burst_now;
            prev_tick_received_at_ = received_at;
            ++last_ring_tail_;
            ++events_since_print;
        }

        ring.tail.store(last_ring_tail_, std::memory_order_release);

        if (events_since_print >= 64) {
            print_book();
            events_since_print = 0;
        }
    }
}

void RuntimeEngine::print_book() const {
    constexpr std::size_t DEPTH = 8;
    PriceLevel bids[DEPTH], asks[DEPTH];
    std::size_t nb = book_.bid_depth(bids, DEPTH);
    std::size_t na = book_.ask_depth(asks, DEPTH);

    // clear screen, cursor to top-left
    printf("\033[2J\033[H");

    printf("  %-12s %-10s %-6s   %-6s %-10s %-12s\n",
           "BID PRICE", "QTY", "#ORD", "#ORD", "QTY", "ASK PRICE");
    printf("  %-12s %-10s %-6s   %-6s %-10s %-12s\n",
           "----------", "-------", "----", "----", "-------", "----------");

    for (std::size_t i = 0; i < DEPTH; ++i) {
        if (i < nb)
            printf("  %-12.6f %-10ld %-6u   ",
                   to_display(bids[i].price), bids[i].total_qty, bids[i].order_count);
        else
            printf("  %-12s %-10s %-6s   ", "", "", "");

        if (i < na)
            printf("%-6u %-10ld %-12.6f",
                   asks[i].order_count, asks[i].total_qty, to_display(asks[i].price));

        printf("\n");
    }

    printf("\n  spread: %.6f  imbalance: %.4f  vwmid: %.6f\n",
           to_display(book_.spread()),
           book_.bid_ask_imbalance(3),
           book_.volume_weighted_mid(3) / PRICE_SCALE);
    printf("  bid_qty: %lu  ask_qty: %lu  ema: %.6f  rate: %.0f ticks/s\n",
           book_.total_bid_qty(), book_.total_ask_qty(),
           to_display(processor_.ema()), processor_.tick_rate());
    fflush(stdout);
}
