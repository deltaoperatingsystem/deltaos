#ifndef ARCH_IO_H
#define ARCH_IO_H

/*
 * architecture-independent I/O interface
 * aach architecture provides its implementation in arch/<arch>/io.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/io.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture - define ARCH_AMD64, ARCH_X86, or ARCH_ARM64"
#endif

/*
 * capability defines - each arch sets these:
 *
 * ARCH_HAS_PORT_IO - 1 if arch has port I/O (x86/amd64/mips e.x)
 * ARCH_HAS_MMIO - 1 if arch has memory-mapped I/O (pretty much all archs)
 *
 * required MI functions - each arch must implement:
 *
 * arch_mmio_read8(addr) - read 8-bit from MMIO address
 * arch_mmio_write8(addr, val) - write 8-bit to MMIO address
 * arch_mmio_read32(addr) - read 32-bit from MMIO address
 * arch_mmio_write32(addr, val)- write 32-bit to MMIO address
 *
 * optional (only if ARCH_HAS_PORT_IO):
 *
 * inb(port) - read 8-bit from I/O port
 * outb(port, val) - write 8-bit to I/O port
 */

#endif
