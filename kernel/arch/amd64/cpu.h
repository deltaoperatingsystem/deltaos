#ifndef ARCH_AMD64_CPU_H
#define ARCH_AMD64_CPU_H

#include <arch/amd64/types.h>

//MI interface implementations

static inline void halt(void) {
    __asm__ volatile ("hlt");
}

static inline void idle(void) {
    __asm__ volatile ("sti; hlt");
}

static inline void pause(void) {
    __asm__ volatile ("pause");
}

//memory barriers

static inline void mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void rmb(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

static inline void wmb(void) {
    __asm__ volatile ("sfence" ::: "memory");
}

//x86-specific

static inline uint64 rdtsc(void) {
    uint32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64)hi << 32) | lo;
}

#endif
