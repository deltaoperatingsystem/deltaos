#ifndef ARCH_AMD64_TIMER_H
#define ARCH_AMD64_TIMER_H

#include <arch/amd64/types.h>

//MI timer interface
void arch_timer_init(uint32 hz);
void arch_timer_setfreq(uint32 hz);
uint32 arch_timer_getfreq(void);
uint64 arch_timer_get_ticks(void);

#endif
