#ifndef ARCH_AMD64_IO_H
#define ARCH_AMD64_IO_H

#include <arch/amd64/types.h>

//amd64 has BOTH port I/O and MMIO
#define ARCH_HAS_PORT_IO 1
#define ARCH_HAS_MMIO    1

//port I/O functions (x86-specific)
uint8 inb(uint16 port);
void outb(uint16 port, uint8 value);

//MMIO functions
static inline uint8 arch_mmio_read8(uintptr addr) {
    return *(volatile uint8*)addr;
}

static inline void arch_mmio_write8(uintptr addr, uint8 val) {
    *(volatile uint8*)addr = val;
}

static inline uint32 arch_mmio_read32(uintptr addr) {
    return *(volatile uint32*)addr;
}

static inline void arch_mmio_write32(uintptr addr, uint32 val) {
    *(volatile uint32*)addr = val;
}

#endif
