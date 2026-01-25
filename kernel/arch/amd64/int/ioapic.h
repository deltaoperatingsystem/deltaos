#ifndef ARCH_AMD64_IOAPIC_H
#define ARCH_AMD64_IOAPIC_H

#include <arch/amd64/types.h>

//I/O APIC register offsets
#define IOAPIC_REG_SEL          0x00
#define IOAPIC_REG_WIN          0x10

//I/O APIC registers (indirect)
#define IOAPIC_ID               0x00
#define IOAPIC_VER              0x01
#define IOAPIC_ARB              0x02
#define IOAPIC_REDTBL_BASE      0x10

//redirection table bits
#define IOAPIC_INT_MASKED       (1 << 16)
#define IOAPIC_TRIGGER_LEVEL    (1 << 15)
#define IOAPIC_INTPOL_LOW       (1 << 13)
#define IOAPIC_DESTMOD_LOGICAL  (1 << 11)

//delivery modes
#define IOAPIC_DELMOD_FIXED     (0 << 8)

#define IOAPIC_DEFAULT_BASE     0xFEC00000

//interface
bool ioapic_init(void);
void ioapic_set_irq(uint8 irq, uint8 vector, uint8 dest_apic_id, bool masked);
void ioapic_mask_irq(uint8 irq);
void ioapic_unmask_irq(uint8 irq);
bool ioapic_is_enabled(void);

#endif
