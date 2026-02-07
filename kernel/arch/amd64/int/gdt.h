#ifndef ARCH_AMD64_GDT_H
#define ARCH_AMD64_GDT_H

#include <types.h>

//GDT segment selectors
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

//RPL (Requested Privilege Level) for user segments
#define GDT_USER_DATA_RPL (GDT_USER_DATA | 3)
#define GDT_USER_CODE_RPL (GDT_USER_CODE | 3)

void gdt_init(void);

//initialize GDT and TSS for an AP
void gdt_init_ap(uint32 cpu_index);

#endif
