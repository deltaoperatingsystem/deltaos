#ifndef MM_MM_H
#define MM_MM_H

#include <arch/types.h>
#include <arch/mmu.h>

//physical to virtual conversion using HHDM
#define P2V(phys) ((void *)((uintptr)(phys) + HHDM_OFFSET))

//virtual to physical conversion using HHDM
#define V2P(virt) ((uintptr)(virt) - HHDM_OFFSET)

#endif
