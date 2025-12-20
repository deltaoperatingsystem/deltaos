#include <types.h>
#include <int/gdt.h>
#include <int/pic.h>
#include <drivers/serial.h>
#include <drivers/keyboard.h>

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

static void irq0_handler(void) {
    serial_write("tick\n");
}

uint64 ticks = 0;

void irq0_handler(void) {
    ticks++;
}

void interrupt_handler(uint64 vector, uint64 error_code) {
    if (vector < 32) {
        serial_write("\nEXCEPTION\n");
        serial_write("Vector:     ");
        serial_write_hex(vector);
        serial_write("\nError Code: ");
        serial_write_hex(error_code);
        serial_write("\n");
        return;
    } else {
        uint8 irq = vector - 32;
        
        switch (irq) {
            case 0:
                irq0_handler();
                break;
            case 1:
                keyboard_irq();
                break;
            default:
                serial_write("Unhandled IRQ: ");
                serial_write_hex(irq);
                serial_write_char('\n');
                break;
        }

        pic_send_eoi(irq);
        return;
    }
}

void idt_setgate(uint8 vector, void *isr, uint8 flags) {
    struct idt_entry *gate = &idt[vector];

    gate->isr_low = (uint64)isr & 0xFFFF;
    gate->kernel_cs = GDT_KERNEL_CODE;
    gate->ist = 0;
    gate->attributes = flags;
    gate->isr_mid = ((uint64)isr >> 16) & 0xFFFF;
    gate->isr_high = ((uint64)isr >> 32) & 0xFFFFFFFF;
    gate->reserved = 0;
}


void idt_init(void) {
    gdt_init();
    idtr.base = (uintptr)&idt[0];
    idtr.limit = (uint16)sizeof(idt) - 1;

    //install handlers for all 256 vectors
    for (int vector = 0; vector < 256; vector++) {
        idt_setgate(vector, isr_stub_table[vector], 0x8E);
    }

    //remap PIC IRQ0-7 -> vectors 32-39, IRQ8-15 -> vectors 40-47
    pic_remap(0x20, 0x28);

    __asm__ volatile ("lidt %0" : : "m"(idtr)); // load the idt
    __asm__ volatile ("sti"); // sets the interrupt flag
}
