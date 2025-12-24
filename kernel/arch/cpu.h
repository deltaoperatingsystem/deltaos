#ifndef ARCH_CPU_H
#define ARCH_CPU_H

/*
 * architecture-independent CPU operations interface
 * each architecture provides its implementation in arch/<arch>/cpu.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/cpu.h>
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
 * arch_halt() - halt CPU until next interrupt
 * arch_idle() - idle CPU (enable interrupts and halt)
 * arch_pause() - hint to CPU that we're in a spin loop
 *
 * memory barriers:
 *
 * arch_mb() - full memory barrier
 * arch_rmb() - read memory barrier
 * arch_wmb() - write memory barrier
 *
 * optional (arch-specific):
 *
 * arch_rdtsc() - read time-stamp counter (x86/amd64)
 */

#endif
