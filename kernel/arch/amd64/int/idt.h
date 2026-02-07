#ifndef ARCH_AMD64_IDT_H
#define ARCH_AMD64_IDT_H

#include <arch/types.h>

void idt_init(void);

//load IDT on current CPU (for AP initialization)
void idt_load(void);

//get IDT pointer (for AP to use same IDT as BSP)
void *idt_get_ptr(void);

#endif