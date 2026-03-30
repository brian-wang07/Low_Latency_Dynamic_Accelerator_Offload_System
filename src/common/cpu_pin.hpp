#pragma once

#include <pthread.h>

#ifdef __APPLE__
    #include <mach/thread_act.h>
    #include <mach/thread_policy.h>
#else
    #include <sched.h>
#endif


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
