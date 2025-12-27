#include <arch/types.h>

void wrmsr(uint32 msr, uint64 value) {
    uint32 lo = value & 0xFFFFFFFF;
    uint32 hi = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

uint64 rdmsr(uint32 msr) {
    uint64 value;
    uint32 lo, hi;

    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));

    value = ((uint64)hi << 32) | lo;
    return value;
}