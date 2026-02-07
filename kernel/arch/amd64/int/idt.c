#include <arch/amd64/types.h>
#include <arch/amd64/interrupts.h>
#include <arch/amd64/timer.h>
#include <arch/mmu.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <lib/io.h>
#include <arch/amd64/context.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/ioapic.h>


struct idt_entry {
	uint16    isr_low;      // The lower 16 bits of the ISR's address
	uint16    kernel_cs;    // The GDT segment selector that the CPU will load into CS before calling the ISR
	uint8	  ist;          // The IST in the TSS that the CPU will load into RSP; set to zero for now
	uint8     attributes;   // Type and attributes; see the IDT page
	uint16    isr_mid;      // The higher 16 bits of the lower 32 bits of the ISR's address
	uint32    isr_high;     // The higher 32 bits of the ISR's address
	uint32    reserved;     // Set to zero
} __attribute__((packed));

#define IDT_MAX_DESCRIPTORS 256

__attribute__((aligned(0x10)))
static struct idt_entry idt[IDT_MAX_DESCRIPTORS];

struct idtr  {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

static struct idtr idtr;
extern void *isr_stub_table[];
extern void arch_timer_tick(void);
extern void sched_tick(int from_usermode);

static void irq0_handler(int from_usermode) {
    arch_timer_tick();
    sched_tick(from_usermode);  //preemptive scheduling - only preempt if from usermode
}


void interrupt_handler(uint64 vector, uint64 error_code, uint64 rip, interrupt_frame_t *frame) {
    if (vector < 32) {
        kpanic(frame, "CPU EXCEPTION (vector 0x%lX, err 0x%lX) at RIP=0x%lX", vector, error_code, rip);
    } else {
        uint8 irq = vector - 32;

        if (apic_is_enabled() && ioapic_is_enabled()) {
            apic_send_eoi();
        } else {
            pic_send_eoi(irq);
        }
        
        //check if we were interrupted from usermode (userspace RIP is low, kernel is high)
        int from_usermode = (rip < 0xFFFF800000000000ULL) ? 1 : 0;
        
        switch (irq) {
            case 0:
                irq0_handler(from_usermode);
                break;
            case 1:
                keyboard_irq();
                break;
            case 12:
                mouse_irq();
                break;
            default: {
                if (vector >= 0x40 && vector <= 0x47) {
                    extern void nvme_isr_callback(uint64);
                    nvme_isr_callback(vector);
                } else {
                    printf("Unhandled IRQ: 0x%X (vector 0x%X)\n", irq + 32, vector);
                }
                break;
            }
        }

        return;
    }
}

void interrupt_mask(uint8 irq) {
    if (apic_is_enabled() && ioapic_is_enabled()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
    }
}

void interrupt_unmask(uint8 irq) {
    if (apic_is_enabled() && ioapic_is_enabled()) {
        ioapic_unmask_irq(irq);
    } else {
        pic_clear_mask(irq);
    }
}

static void idt_setgate(uint8 vector, void *isr, uint8 flags, uint8 ist) {
    struct idt_entry *gate = &idt[vector];

    gate->isr_low = (uint64)isr & 0xFFFF;
    gate->kernel_cs = GDT_KERNEL_CODE;
    gate->ist = ist;  //IST index (1-7) or 0 for no IST
    gate->attributes = flags;
    gate->isr_mid = ((uint64)isr >> 16) & 0xFFFF;
    gate->isr_high = ((uint64)isr >> 32) & 0xFFFFFFFF;
    gate->reserved = 0;
}

void arch_interrupts_init(void) {
    gdt_init();
    idtr.base = (uintptr)&idt[0];
    idtr.limit = (uint16)sizeof(idt) - 1;

    //install handlers for all 256 vectors.
    //we avoid using IST for general interrupts to prevent "leaking" execution onto the shared IST stack
    //IST is only used for critical exceptions like double fault (vector 8)
    for (int vector = 0; vector < 256; vector++) {
        uint8 ist = (vector == 8) ? 1 : 0;
        idt_setgate(vector, isr_stub_table[vector], 0x8E, ist);
    }

    //remap PIC IRQ0-7 -> vectors 32-39, IRQ8-15 -> vectors 40-47
    pic_remap(0x20, 0x28);

    //try to initialize APIC
    if (apic_init()) {
        //unmask keyboard and mouse
        interrupt_unmask(1);
        interrupt_unmask(12);
        printf("[amd64] APIC/IOAPIC initialized\n");
    } else {
        //fallback to PIC
        interrupt_unmask(2); //unmask cascade IRQ 2 for slave PIC
        interrupt_unmask(1); //keyboard
        interrupt_unmask(12); //mouse
        printf("[amd64] Using legacy PIC\n");
    }

    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void arch_interrupts_enable(void) {
    __asm__ volatile ("sti");
}

void arch_interrupts_disable(void) {
    __asm__ volatile ("cli");
}

void idt_load(void) {
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void *idt_get_ptr(void) {
    return &idtr;
}
