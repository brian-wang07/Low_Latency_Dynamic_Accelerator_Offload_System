#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "../common/shm_manager.hpp"
#include "../common/shm_types.hpp"
#include "data_generator.hpp"

static volatile sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

int main() {
  //catch potential ctrl c / explicit sigint and sigterm syscalls, to ensure that raii destructor is explicitly called.
  //if this doesnt work, just do rm dev/shm/engine_shm_mvp
  struct sigaction sa{};
  sa.sa_handler = handle_signal;
  sigaction(SIGINT,  &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  ShmManager shm;
  DataGeneratorConfig cfg{
      .base_interval_ns = 100'000,          // 0.1 ms
      .burst_interval_ns = 1'000,           // 0.001 ms
      .burst_length = 5,
      .start_price = 100.0,
      .volatility = 0.05, 
      .drift = 2e-6,                        // ~+1 per 5000 ticks at price 100
      .burst_probability = 0.05,
      .burst_volatility_multiplier = 0.5,
      .seed = 42,
      .jitter_sigma = 0.2,
      .enable_bursts = true,
      .spread_mean            = 0.10,
      .spread_reversion_speed = 5.0,
      .spread_volatility      = 0.02,
      .min_spread             = 0.01,
      .start_spread           = 0.10,
      .depth_log_mean         = 500,
      .depth_log_sigma        = 1,
      .prob_limit             = 0.70,
      .prob_bid               = 0.50,
      .prob_new               = 0.60,
      .prob_cancel            = 0.0,
  };


  if (shm.create()) {
    DataGenerator generator(cfg);
    using clock = std::chrono::steady_clock;
    auto next_time = clock::now();
    while (g_running) {

      auto tick = generator.next();

      auto *block = shm.as<engine::shm::SharedMemoryBlock>();
        
      auto& md = block->latest_market_data;
      md.bid.store       (tick.data.bid,                                          std::memory_order_relaxed);
      md.ask.store       (tick.data.ask,                                          std::memory_order_relaxed);
      md.depth.store     (tick.data.depth,                                        std::memory_order_relaxed);
      md.price.store     ((tick.data.bid + tick.data.ask) / 2.0,                  std::memory_order_relaxed);
      md.order_type.store(static_cast<uint8_t>(tick.data.order_type),             std::memory_order_relaxed);
      md.side.store      (static_cast<uint8_t>(tick.data.side),                   std::memory_order_relaxed);
      md.action.store    (static_cast<uint8_t>(tick.data.action),                 std::memory_order_relaxed);
      md.timestamp.store (std::chrono::duration_cast<std::chrono::nanoseconds>(
                            clock::now().time_since_epoch()).count(),              std::memory_order_relaxed);
      // sequence_number written last with release — establishes happens-before
      // so the runtime's acquire load on seq makes all prior relaxed stores safe to read
      md.sequence_number.store(tick.data.sequence_number, std::memory_order_release);

      std::cout
          << "seq="    << tick.data.sequence_number
          << " price= " << tick.data.price
          << " bid="   << tick.data.bid
          << " ask="   << tick.data.ask
          << " sprd="  << (tick.data.ask - tick.data.bid)
          << " depth=" << tick.data.depth
          << " type="  << (tick.data.order_type == OrderType::LIMIT ? "L" : "M")
          << " side="  << (tick.data.side == Side::BID ? "B" : "A")
          << " act="   << (tick.data.action == OrderAction::NEW ? "N"
                         : tick.data.action == OrderAction::CANCEL ? "C" : "D")
          << " burst=" << tick.in_burst
          << '\n';

      next_time += std::chrono::nanoseconds(tick.wait_ns);


      // hybrid wait
      while (g_running) {
          auto now = clock::now();
          if (now >= next_time) break;

          auto remaining = next_time - now;

          if (remaining > std::chrono::microseconds(50)) {
              std::this_thread::sleep_for(remaining - std::chrono::microseconds(20));
          }
      }
    }
  }

  else {
    std::cerr << "Failed to create shared memory\n";
    return 1;
  }

  return 0;

}