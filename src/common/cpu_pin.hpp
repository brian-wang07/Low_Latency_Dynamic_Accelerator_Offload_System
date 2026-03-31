#pragma once

#include <pthread.h>

#ifdef __APPLE__
    #include <mach/thread_act.h>
    #include <mach/thread_policy.h>
#else
    #include <sched.h>
#endif

//| Core | Thread | Notes |
//|----- |--------|-------|
//|  0   | Matching Engine | Hottest — polls all input rings |
//|  1   | Data Dispatcher | Clock-driven, sleeps between events |
//|  2   | Runtime Hot Path | Book reconstruction + strategy feed |
//|  3   | Strategy | Decision loop |
//|  4   | Data Creator | Batch precompute, not latency-critical |
//|  5   | Snapshotter + Dashboard | Cool threads, can share |

//TODO: Fix sibling cores; cores (0, 1) share cpu 0. maybe this is fine tho? idk check

inline bool pin_to_core(int core_id) noexcept {
#ifdef __APPLE__
    thread_affinity_policy_data_t policy = { core_id };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                            reinterpret_cast<thread_policy_t>(&policy),
                            THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;

#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;

#endif
}
