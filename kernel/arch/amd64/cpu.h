#ifndef ARCH_AMD64_CPU_H
#define ARCH_AMD64_CPU_H

#include <arch/amd64/types.h>
#include <arch/amd64/percpu.h>

//kernel stack for ring transitions (wraps TSS RSP0)
void arch_set_kernel_stack(void *stack_top);


//MI interface implementations

static inline void arch_halt(void) {
    __asm__ volatile ("hlt");
}

static inline void arch_idle(void) {
    __asm__ volatile ("sti; hlt");
}

static inline void arch_pause(void) {
    __asm__ volatile ("pause");
}

//memory barriers

static inline void arch_mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void arch_rmb(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

static inline void arch_wmb(void) {
    __asm__ volatile ("sfence" ::: "memory");
}

//x86-specific

static inline uint64 arch_rdtsc(void) {
    uint32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64)hi << 32) | lo;
}

#endif
