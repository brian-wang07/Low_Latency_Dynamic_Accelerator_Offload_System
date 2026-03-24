#include <chrono>
#include <iostream>
#include <thread>

#include "../common/shm_manager.hpp"
#include "../common/shm_types.hpp"
#include "data_generator.hpp"

///Shared memory manager implementation. The manager is to be owned by the data generator, 
///as it is the first thing that gets created that uses the shared memory.

int main() {
  ShmManager shm;
  DataGeneratorConfig cfg{
      .base_interval_ns = 100'000,          // 0.1 ms
      .burst_interval_ns = 1'000,           // 0.001 ms
      .burst_length = 5,
      .start_price = 100.0,
      .volatility = 0.5,
      .burst_probability = 0.1,
      .burst_volatility_multiplier = 3.0,
      .seed = 42,
      .jitter_sigma = 0.2,
      .enable_bursts = true
  };


  if (shm.create()) {
    DataGenerator generator(cfg);
    using clock = std::chrono::steady_clock;
    auto next_time = clock::now();
    while (1) {

      auto tick = generator.next();

      auto *block = shm.as<engine::shm::SharedMemoryBlock>();
        
      block->latest_market_data.price.store(tick.data.price, std::memory_order_relaxed);
      block->latest_market_data.timestamp.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch()).count(),
          std::memory_order_relaxed);
      // sequence_number written last with release — establishes happens-before
      // so the runtime's acquire load on seq makes price/timestamp safe to read relaxed
      block->latest_market_data.sequence_number.store(tick.data.sequence_number, std::memory_order_release);      


      std::cout
          << "seq=" << tick.data.sequence_number
          << " price=" << tick.data.price
          << " wait_ms=" << tick.wait_ns
          << " burst=" << tick.in_burst
          << '\n';

      next_time += std::chrono::nanoseconds(tick.wait_ns);


      // hybrid wait
      while (1) {
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