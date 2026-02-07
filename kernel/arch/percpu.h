#ifndef ARCH_PERCPU_H
#define ARCH_PERCPU_H

/*
 * architecture-independent interface for per-CPU data
 * each architecture provides its implementation in arch/<arch>/percpu.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/percpu.h>
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
 * percpu_t *percpu_get(void) - get pointer to current CPU's per-CPU data
 * percpu_t *percpu_get_by_index(uint32 index) - get percpu by logical CPU index
 * void percpu_init(void) - initialize per-CPU data for the boot CPU (BSP)
 * void percpu_init_ap(uint32 cpu_index, uint32 apic_id) - initialize per-CPU for an AP
 * void percpu_set_kernel_stack(void *stack_top) - set current CPU's kernel stack
 * uint32 percpu_cpu_count(void) - get total number of CPUs detected
 */

#endif
