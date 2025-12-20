#ifndef ARCH_AMD64_GDT_H
#define ARCH_AMD64_GDT_H

#include <types.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

void gdt_init(void);

#endif
