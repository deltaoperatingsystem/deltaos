#ifndef ARCH_AMD64_TSS_H
#define ARCH_AMD64_TSS_H

#include <arch/types.h>

//64-bit Task State Segment
//used for Ring 3 -> Ring 0 transitions (RSP0) and interrupt stacks (IST)
typedef struct {
    uint32 reserved0;
    uint64 rsp0;            //stack pointer for Ring 0
    uint64 rsp1;            //stack pointer for Ring 1 (unused)
    uint64 rsp2;            //stack pointer for Ring 2 (unused)
    uint64 reserved1;
    uint64 ist[7];          //interrupt stack table entries
    uint64 reserved2;
    uint16 reserved3;
    uint16 iopb_offset;     //I/O permission bitmap offset
} __attribute__((packed)) tss_t;

//set the Ring 0 stack pointer (call on context switch to userspace process)
void tss_set_rsp0(uint64 rsp);

//get the TSS (for GDT setup)
tss_t *tss_get(void);

#endif
