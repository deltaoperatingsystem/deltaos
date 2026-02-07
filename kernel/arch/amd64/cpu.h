#ifndef ARCH_AMD64_CPU_H
#define ARCH_AMD64_CPU_H

#include <arch/amd64/types.h>

//kernel stack for ring transitions (wraps TSS RSP0)
void arch_set_kernel_stack(void *stack_top);


//MI interface implementations
uint32 arch_cpu_index(void);
uint32 arch_cpu_count(void);

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

static inline void arch_cpuid(uint32 leaf, uint32 subleaf, uint32 *eax, uint32 *ebx, uint32 *ecx, uint32 *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

//interrupt state save/restore (for nested critical sections)
typedef uint64 irq_state_t;

static inline irq_state_t arch_irq_save(void) {
    irq_state_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void arch_irq_restore(irq_state_t flags) {
    __asm__ volatile ("push %0; popfq" :: "r"(flags) : "memory");
}

#endif
