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
 * arch_set_kernel_stack(void *stack_top) - set kernel stack for ring transitions
 * arch_cpu_index() - get the current CPU logical index/ID
 *
 * memory barriers:
 *
 * arch_mb() - full memory barrier
 * arch_rmb() - read memory barrier
 * arch_wmb() - write memory barrier
 *
 * interrupt state (for nested critical sections):
 *
 * irq_state_t arch_irq_save() - disable interrupts and return previous state
 * arch_irq_restore(irq_state_t) - restore interrupt state
 *
 * optional (arch-specific):
 *
 * arch_rdtsc() - read time-stamp counter (x86/amd64)
 * arch_cpuid(leaf, subleaf, *eax, *ebx, *ecx, *edx) - CPU identification (x86/amd64)
 */

#endif
