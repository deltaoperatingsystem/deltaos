#ifndef ARCH_AMD64_INTERRUPTS_H
#define ARCH_AMD64_INTERRUPTS_H

#include <arch/amd64/types.h>

//MI interface
void arch_interrupts_init(void);
void arch_interrupts_enable(void);
void arch_interrupts_disable(void);

//MD internals
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

void gdt_init(void);
void pic_send_eoi(uint8 irq);
void pic_remap(uint8 vector1, uint8 vector2);
void pic_disable(void);
void pic_set_mask(uint8 irqline);
void pic_clear_mask(uint8 irqline);

#endif
