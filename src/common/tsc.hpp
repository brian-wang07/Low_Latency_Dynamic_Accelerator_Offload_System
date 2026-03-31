#pragma once


// Cross-platform timestamp counter read.
// x86/x64: __rdtsc() via intrinsic
// ARM/AArch64: CNTVCT_EL0 (virtual timer count register)

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #ifdef _MSC_VER
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
    #define READ_TSC() __rdtsc()
#elif defined(__aarch64__) || defined(_M_ARM64)
    static inline uint64_t read_cntvct() {
        uint64_t val;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
    }
    #define READ_TSC() read_cntvct()
#else
    #error "Unsupported architecture: no TSC / cycle counter available"
#endif
