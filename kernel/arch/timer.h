#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

/*
 * architecture-independent timer interface
 * each architecture provides its implementation in arch/<arch>/timer.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/timer.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture"
#endif

/*
 * required MI functions - each arch must implement:
 *
 * arch_timer_init(hz) - initialize timer at given frequency
 * arch_timer_setfreq(hz) - change timer frequency
 * arch_timer_get_ticks() - get monotonic tick count since boot
 */

#endif
