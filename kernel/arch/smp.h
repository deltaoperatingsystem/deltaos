#ifndef ARCH_SMP_H
#define ARCH_SMP_H

/*
 * architecture-independent interface for SMP operations
 * each architecture provides its implementation in arch/<arch>/smp.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/smp/smp.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture"
#endif

#define IPI_RESCHEDULE   0xFD
#define IPI_TLB_SHOOTDOWN 0xFE

/*
required MI functions - each arch must implement in its specific header:
- void smp_init(void);
- uint32 smp_cpu_count(void);
- bool smp_ap_started(uint32 cpu_index);
- void arch_smp_send_resched(uint32 cpu_index);
*/

#endif
