#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

/*
 * architecture-independent IRQ composition interface
 * each architecture provides its implementation in arch/<arch>/irq.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/irq.h>
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
 * irq_compose_msi(vector, *msg) - compose MSI address/data for a vector
 */

#endif
