#ifndef ARCH_POWER_H
#define ARCH_POWER_H

/*
 * architecture-independent power interface
 * each architecture provides its implementation in arch/<arch>/power.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/power.h>
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
 * arch_power_reboot() - reboot the system
 * arch_power_shutdown() - power off the system
 */

#endif
