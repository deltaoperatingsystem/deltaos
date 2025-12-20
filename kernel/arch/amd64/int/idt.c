#include <types.h>

struct idt_entry {
	uint16    isr_low;      // The lower 16 bits of the ISR's address
	uint16    kernel_cs;    // The GDT segment selector that the CPU will load into CS before calling the ISR
	uint8	  ist;          // The IST in the TSS that the CPU will load into RSP; set to zero for now
	uint8     attributes;   // Type and attributes; see the IDT page
	uint16    isr_mid;      // The higher 16 bits of the lower 32 bits of the ISR's address
	uint32    isr_high;     // The higher 32 bits of the ISR's address
	uint32    reserved;     // Set to zero
} __attribute__((packed));

#define IDT_MAX_DESCRIPTORS 255

__attribute__((aligned(0x10)))
static struct idt_entry idt[IDT_MAX_DESCRIPTORS + 1];

struct idtr  {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

static struct idtr idtr;

__attribute__((noreturn))
void exception_handler(void) {
    __asm__ volatile ("cli; hlt"); // hangs the cpu on error
}


#define GDT_OFFSET_KERNEL_CODE 0

void idt_set_descriptor(uint8 vector, void *isr, uint8 flags) {
    struct idt_entry *descriptor = &idt[vector];

    descriptor->isr_low = (uint64)isr & 0xFFFF;
    descriptor->kernel_cs = GDT_OFFSET_KERNEL_CODE; // TODO: GDT
    descriptor->ist = 0;
    descriptor->attributes = flags;
    descriptor->isr_mid = ((uint64)isr >> 16) & 0xFFFF;
    descriptor->isr_high = ((uint64)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved = 0;
}

static bool vectors[IDT_MAX_DESCRIPTORS];

extern void *isr_stub_table[];

void idt_init(void) {
    idtr.base = (uintptr)&idt[0];
    idtr.limit = (uint16)sizeof(struct idt_entry) * IDT_MAX_DESCRIPTORS - 1;

    for (uint8 vector = 0; vector < 32; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
        vectors[vector] = true;
    }

    __asm__ volatile ("lidt %0" : : "m"(idtr)); // load the idt
    __asm__ volatile ("sti"); // sets the interrupt flag
}