#ifndef ARCH_INTERRUPTS_H
#define ARCH_INTERRUPTS_H

/*
 * architecture-independent interrupt and timer interface
 * each architecture provides its implementation in arch/<arch>/interrupts.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/interrupts.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture - define ARCH_AMD64, ARCH_X86, or ARCH_ARM64"
#endif

/*
 * required MI functions - each arch must implement:
 *
 * arch_interrupts_init() - initialize interrupt controller (IDT/GIC/etc)
 * arch_interrupts_enable() - enable interrupts (sti/cpsie/etc)
 * arch_interrupts_disable() - disable interrupts (cli/cpsid/etc)
 * arch_timer_init(hz) - initialize timer at given frequency
 * arch_timer_setfreq(hz) - change timer frequency
 */

#endif
