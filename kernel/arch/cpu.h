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
 * halt() - halt CPU until next interrupt
 * idle() - idle CPU (enable interrupts and halt)
 * pause() - hint to CPU that we're in a spin loop
 *
 * memory barriers:
 *
 * mb() - full memory barrier
 * rmb() - read memory barrier
 * wmb() - write memory barrier
 *
 * optional (arch-specific):
 *
 * rdtsc() - read time-stamp counter (x86/amd64)
 */

#endif
