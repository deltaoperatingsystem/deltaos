#ifndef ARCH_AMD64_PIT_H
#define ARCH_AMD64_PIT_H

#include <types.h>

void pit_setfreq(uint32 hz);
void pit_init(uint32 hz);

#endif