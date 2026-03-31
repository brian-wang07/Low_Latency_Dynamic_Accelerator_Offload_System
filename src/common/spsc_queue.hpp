// local heap allocated SPSC ring for Creator -> Dispatch

#include <atomic>
#include <cstddef>
#include <cstdint>

template <typename T, uint32_t capacity>
struct alignas(64) SpscQueue {
    static_assert((capacity & (capacity - 1)) == 0);
    static constexpr uint32_t MASK = capacity - 1;
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
    alignas(64) T slots[capacity];
    bool try_push(const T& item) noexcept;
    bool try_pop(T& out) noexcept;
    uint32_t size() const noexcept;
};

