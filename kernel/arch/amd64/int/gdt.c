#include <arch/amd64/interrupts.h>
#include <arch/amd64/int/tss.h>
#include <lib/io.h>

//GDT entry (8 bytes)
struct gdt_entry {
    uint16 limit_low;
    uint16 base_low;
    uint8  base_mid;
    uint8  access;
    uint8  granularity;
    uint8  base_high;
} __attribute__((packed));

//GDT entry for TSS (16 bytes in long mode)
struct gdt_tss_entry {
    uint16 limit_low;
    uint16 base_low;
    uint8  base_mid;
    uint8  access;
    uint8  granularity;
    uint8  base_high;
    uint32 base_upper;
    uint32 reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

//GDT: null + kernel code/data + user data/code + TSS (2 entries)
static struct gdt_entry gdt[7];
static struct gdt_ptr gp;

//TSS instance
static tss_t tss;

static void gdt_set_gate(int num, uint32 base, uint32 limit, uint8 access, uint8 gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_mid = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;

    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access = access;
}

static void gdt_set_tss(int num, uint64 base, uint32 limit) {
    struct gdt_tss_entry *tss_entry = (struct gdt_tss_entry *)&gdt[num];
    
    tss_entry->limit_low = limit & 0xFFFF;
    tss_entry->base_low = base & 0xFFFF;
    tss_entry->base_mid = (base >> 16) & 0xFF;
    tss_entry->access = 0x89;  //present 64-bit TSS available
    tss_entry->granularity = (limit >> 16) & 0x0F;
    tss_entry->base_high = (base >> 24) & 0xFF;
    tss_entry->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_entry->reserved = 0;
}

void gdt_init(void) {
    //initialize TSS
    for (size i = 0; i < sizeof(tss); i++) {
        ((uint8 *)&tss)[i] = 0;
    }
    tss.iopb_offset = sizeof(tss_t);  //no I/O bitmap

    //set up GDT pointer (7 entries but TSS takes 2 slots)
    gp.limit = (sizeof(struct gdt_entry) * 7) - 1;
    gp.base = (uintptr)&gdt;

    //index 0: null segment
    gdt_set_gate(0, 0, 0, 0, 0);
    
    //index 1: kernel code (selector 0x08)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
    
    //index 2: kernel data (selector 0x10)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    
    //index 3: user data (selector 0x18) - DPL=3
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    //index 4: user code (selector 0x20) - DPL=3, 64-bit
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xFA, 0xAF);
    
    //index 5-6: TSS (selector 0x28) - spans 2 entries
    gdt_set_tss(5, (uint64)&tss, sizeof(tss_t) - 1);
    
    //allocate IST1 stack (used for timer/all interrupts to get consistent frame)
    //using a static buffer for nowww
    static uint8 ist1_stack[16384] __attribute__((aligned(16)));
    tss.ist[0] = (uint64)&ist1_stack[sizeof(ist1_stack)];  //stack grows down

    //load GDT
    __asm__ volatile ("lgdt %0" : : "m"(gp));

    //reload segment registers
    __asm__ volatile (
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );

    //load TSS
    __asm__ volatile ("ltr %w0" : : "r"((uint16)0x28));
    
    puts("[gdt] initialized with TSS and IST1\n");
}

void tss_set_rsp0(uint64 rsp) {
    tss.rsp0 = rsp;
}

tss_t *tss_get(void) {
    return &tss;
}

//set kernel stack for ring 3 -> ring 0 transitions
void arch_set_kernel_stack(void *stack_top) {
    tss_set_rsp0((uint64)stack_top);
}
