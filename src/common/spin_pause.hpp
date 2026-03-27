#pragma once

// Cross-platform spin-loop hint.
// x86/x64: _mm_pause() (PAUSE instruction — reduces pipeline contention)
// ARM/AArch64: YIELD instruction (equivalent hint for spin-wait loops)

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define SPIN_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define SPIN_PAUSE() __asm__ volatile("yield")
#elif defined(__arm__) || defined(_M_ARM)
    #define SPIN_PAUSE() __asm__ volatile("yield")
#else
    // Fallback: compiler barrier, no hint
    #define SPIN_PAUSE() ((void)0)
#endif
