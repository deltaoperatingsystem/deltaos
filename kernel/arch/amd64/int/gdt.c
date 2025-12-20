#include "gdt.h"

struct gdt_entry {
    uint16 limit_low;
    uint16 base_low;
    uint8  base_mid;
    uint8  access;
    uint8  granularity;
    uint8  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr gp;

static void gdt_set_gate(int num, uint32 base, uint32 limit, uint8 access, uint8 gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_mid = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;

    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
    gp.base = (uintptr)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                //null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); //code segment (64-bit)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); //data segment

    __asm__ volatile ("lgdt %0" : : "m"(gp));

    //reload segment registers with new GDT selectors
    //CS requires a far return and data segments can just be mov'd
    __asm__ volatile (
        "mov $0x10, %%ax\n\t"   //data selector
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"    //null for fs/gs
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        //far return to reload CS
        "pushq $0x08\n\t"       //code selector
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );
}
