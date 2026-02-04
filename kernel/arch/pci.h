#ifndef ARCH_PCI_H
#define ARCH_PCI_H

#include <arch/types.h>

/*
 * architecture-independent PCI configuration space interface
 * each architecture provides its implementation in arch/<arch>/pci.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/pci.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture"
#endif

/*
 * required MI functions - archs that support PCI must implement:
 *
 * arch_pci_read(bus, dev, func, offset, size) - read from config space
 * arch_pci_write(bus, dev, func, offset, size, value) - write to config space
 */

#endif
