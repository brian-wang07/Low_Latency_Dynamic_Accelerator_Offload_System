#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "shm_manager.hpp"
#include "shm_types.hpp"

struct CachedTick {
    uint64_t sequence_number;
    double   price;
    uint64_t data_timestamp_ns;  // from MarketData::timestamp
    uint64_t received_at_ns;     // steady_clock::now() at observation
};

template<typename T, std::size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
public:
    void push(const T& item) {
        buf_[head_ & (N - 1)] = item;
        ++head_;
    }
    std::size_t count() const noexcept { return head_ < N ? head_ : N; }
    const T& operator[](std::size_t i) const noexcept { return buf_[i & (N - 1)]; }
private:
    std::array<T, N> buf_{};
    std::size_t head_ = 0;
};

class TickProcessor {
public:
    explicit TickProcessor(double alpha = 0.05, int print_every = 1000);
    void on_tick(const CachedTick& tick);

private:
    double alpha_;
    int    print_every_;
    double ema_ = 0.0;
    bool   first_ = true;
    int    tick_count_ = 0;
    uint64_t window_start_ns_ = 0;
    int    ticks_in_window_ = 0;
    double tick_rate_ = 0.0;
};

class RuntimeEngine {
public:
    void run();

private:
    ShmManager shm_;
    RingBuffer<CachedTick, 1024> cache_;
    TickProcessor processor_;
};
