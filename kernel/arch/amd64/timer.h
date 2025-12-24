#ifndef ARCH_AMD64_TIMER_H
#define ARCH_AMD64_TIMER_H

#include <arch/amd64/types.h>

//MI timer interface
void timer_init(uint32 hz);
void timer_setfreq(uint32 hz);
uint32 timer_getfreq(void);
uint64 timer_get_ticks(void);

#endif
