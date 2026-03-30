/// This header defines all of the common types used throughout.
/// Implementation architecture:
/// ┌────────────────────────────────────────────────┐                                                                   
/// │              EXCHANGE (Process 1)              │                                                                   
/// │                                                │                                                                   
/// │     [CREATOR]───────HEAP────────►[DISPATCHER]──┼─────────┐ 
/// │                                                │         │ 
/// │   [ADVERSARIES]────────────────────────────────┼──────┐  │  Each producer will have its own spsc ring, and k-way merge                        
/// │                                                │     ┌▼──▼┐ on the enqueue timestamps.
/// │ [MATCHING ENGINE]◄─────────────────────────────┼─────┤SPSC◄─────┐                                                  
/// │         │                                      │     └────┘     │                                                  
/// └─────────┼──────────────────────────────────────┘    EXCHANGE    │                                                  
///        ┌──▼─┐                                                     │                                                  
///        │SPSC│ENGINE                                               │                                                  
///        └──┬─┘                                                     │                                                  
/// ┌─────────┼──────────────────────────────────────┐                │                                                  
/// │         │ RUNTIME ENGINE (Process 2)           │                │                                                  
/// │         │                                      │ DASHBOARD      │                                                  
/// │         ▼         ┌───────┐                    │ ┌────┐         │                                                  
/// │    [HOT PATH]─────►Seqlock┼────►[SNAPSHOTTER]──┼─►SPSC│         │                                                  
/// │         │         └───────┘                    │ └──┬─┘         │                                                  
/// └─────────┼──────────────────────────────────────┘    │           │                                                  
///           │                                           │           │                                                  
///        ┌──▼─┐                      ┌──────────────────┴──┐        │                                                  
///        │SPSC│STRATEGY              │DASHBOARD (Process 4)│        │                                                  
///        └──┬─┘                      └─────────────────────┘        │                                                  
///           │                                                       │                                                  
/// ┌─────────┼──────────────────────────────────────┐                │                                                  
/// │         │    STRATEGY (Process 3)              │                │                                                  
/// │         ▼   Dispatch to main or offload        │                │                                                  
/// │   [DISPATCHER]───────────────────────►[MAIN]   │                │                                                  
/// │         │                                │     │                │                                                  
/// │         ▼                                │     │                │                                                  
/// │   [ACCELERATOR]──────────────────────────┴─────┼────────────────┘                                                  
/// │                                                │                                                                   
/// └────────────────────────────────────────────────┘                                                                    


#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "dashboard_shm.hpp"
#include "exchange_shm.hpp"
#include "runtime_shm.hpp"
#include "strategy_shm.hpp"
#include "shm_core.hpp"
#include "accelerator_types.hpp"


namespace common::shm {
struct SharedMemoryBlock {
    alignas(64) common::shm::ShmHeader            header;
    alignas(64) exchange::shm::ExchangeInputArray exchange_feed;
    alignas(64) runtime::shm::EventRingBuffer     market_data_feed;
    alignas(64) strategy::shm::StrategyRingBuffer strategy_feed;
    alignas(64) dashboard::shm::BookSnapshot      dashboard_snapshot;
    alignas(64) accelerator::AcceleratorBatch     accelerator_feed;
    alignas(64) accelerator::AcceleratorSignal    accelerator_signal;
};

inline void shm_init_header(SharedMemoryBlock *block) noexcept {
    block->header.magic   = SHM_MAGIC;
    block->header.version = SHM_VERSION;
}

[[nodiscard]] 
inline bool shm_validate_header(const SharedMemoryBlock *block) noexcept {
    return block->header.magic   == SHM_MAGIC
        && block->header.version == SHM_VERSION;
}

} // namespace engine::shm

static_assert(offsetof(common::shm::SharedMemoryBlock, header) % 64 == 0);
static_assert(offsetof(common::shm::SharedMemoryBlock, exchange_feed) % 64 == 0);
static_assert(offsetof(common::shm::SharedMemoryBlock, market_data_feed) % 64 == 0);
static_assert(offsetof(common::shm::SharedMemoryBlock, strategy_feed) % 64 == 0);
static_assert(offsetof(common::shm::SharedMemoryBlock, dashboard_snapshot) % 64 == 0);
static_assert(offsetof(common::shm::SharedMemoryBlock, accelerator_feed) % 64 == 0);
static_assert(offsetof(common::shm::SharedMemoryBlock, accelerator_signal) % 64 == 0);
static_assert(sizeof(common::shm::SharedMemoryBlock) <= common::shm::SHM_SIZE);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int8_t>::is_always_lock_free);
static_assert(std::atomic<uint8_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);


static_assert((runtime::shm::EVENT_RING_CAPACITY & (runtime::shm::EVENT_RING_CAPACITY - 1))         == 0, "must be power of 2");
static_assert((exchange::shm::EXCHANGE_RING_CAPACITY & (exchange::shm::EXCHANGE_RING_CAPACITY - 1)) == 0, "must be power of 2");
static_assert((strategy::shm::STRATEGY_RING_CAPACITY & (strategy::shm::STRATEGY_RING_CAPACITY - 1)) == 0, "must be power of 2");



