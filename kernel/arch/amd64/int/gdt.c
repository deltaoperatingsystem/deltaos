#include <arch/amd64/interrupts.h>
#include <arch/amd64/percpu.h>
#include <arch/amd64/int/tss.h>
#include <arch/amd64/int/gdt.h>
#include <lib/io.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <arch/io.h>

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

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

//BSP GDT: null + kernel code/data + user data/code + TSS (2 entries)
static struct gdt_entry gdt[7];
static struct gdt_ptr gp;

//BSP TSS instance
static tss_t tss;

//per-CPU GDT and TSS for APs (allocated dynamically)
#define AP_IST_STACK_SIZE 16384

static void gdt_set_gate_on(struct gdt_entry *entries, int num, uint32 base, uint32 limit, uint8 access, uint8 gran) {
    entries[num].base_low = (base & 0xFFFF);
    entries[num].base_mid = (base >> 16) & 0xFF;
    entries[num].base_high = (base >> 24) & 0xFF;

    entries[num].limit_low = (limit & 0xFFFF);
    entries[num].granularity = (limit >> 16) & 0x0F;

    entries[num].granularity |= gran & 0xF0;
    entries[num].access = access;
}

static void gdt_set_gate(int num, uint32 base, uint32 limit, uint8 access, uint8 gran) {
    gdt_set_gate_on(gdt, num, base, limit, access, gran);
}

static void gdt_set_tss_on(struct gdt_entry *entries, int num, uint64 base, uint32 limit) {
    struct gdt_tss_entry *tss_entry = (struct gdt_tss_entry *)&entries[num];
    
    tss_entry->limit_low = limit & 0xFFFF;
    tss_entry->base_low = base & 0xFFFF;
    tss_entry->base_mid = (base >> 16) & 0xFF;
    tss_entry->access = 0x89;  //present 64-bit TSS available
    tss_entry->granularity = (limit >> 16) & 0x0F;
    tss_entry->base_high = (base >> 24) & 0xFF;
    tss_entry->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_entry->reserved = 0;
}

static void gdt_set_tss(int num, uint64 base, uint32 limit) {
    gdt_set_tss_on(gdt, num, base, limit);
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

    //restore GS base because mov to gs clobbers it
    percpu_t *bsp_ptr = percpu_get_by_index(0);
    wrmsr(IA32_GS_BASE, (uint64)bsp_ptr);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64)bsp_ptr);


    //load TSS
    __asm__ volatile ("ltr %w0" : : "r"((uint16)0x28));
    
    //store BSP TSS in percpu
    percpu_t *bsp = percpu_get_by_index(0);
    if (bsp) {
        bsp->tss = (struct tss *)&tss;
        bsp->gdt = gdt;
        bsp->ist_stack = ist1_stack;
    }
    
    puts("[gdt] initialized with TSS and IST1\n");
}

void gdt_init_ap(uint32 cpu_index) {
    //allocate per-CPU GDT, TSS and IST stack
    struct gdt_entry *ap_gdt = kmalloc(sizeof(struct gdt_entry) * 7);
    tss_t *ap_tss = kmalloc(sizeof(tss_t));
    uint8 *ap_ist = kmalloc(AP_IST_STACK_SIZE);
    
    if (!ap_gdt || !ap_tss || !ap_ist) {
        printf("[gdt] ERR: Failed to allocate AP %u GDT/TSS\n", cpu_index);
        return;
    }
    
    //initialize TSS
    memset(ap_tss, 0, sizeof(tss_t));
    ap_tss->iopb_offset = sizeof(tss_t);
    ap_tss->ist[0] = (uint64)(ap_ist + AP_IST_STACK_SIZE); //stack grows down
    
    //copy standard GDT entries from BSP
    gdt_set_gate_on(ap_gdt, 0, 0, 0, 0, 0);
    gdt_set_gate_on(ap_gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
    gdt_set_gate_on(ap_gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate_on(ap_gdt, 3, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    gdt_set_gate_on(ap_gdt, 4, 0, 0xFFFFFFFF, 0xFA, 0xAF);
    
    //set TSS entry pointing to this AP's TSS
    gdt_set_tss_on(ap_gdt, 5, (uint64)ap_tss, sizeof(tss_t) - 1);
    
    //set up GDT pointer for this AP
    struct gdt_ptr ap_gp;
    ap_gp.limit = (sizeof(struct gdt_entry) * 7) - 1;
    ap_gp.base = (uintptr)ap_gdt;
    
    //load GDT
    __asm__ volatile ("lgdt %0" : : "m"(ap_gp));
    
    //reload segment registers
    __asm__ volatile (
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        ::: "rax", "memory"
    );
    
    //restore GS base for this AP
    percpu_t *ap_ptr = percpu_get_by_index(cpu_index);
    wrmsr(IA32_GS_BASE, (uint64)ap_ptr);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64)ap_ptr);

    
    //load TSS
    __asm__ volatile ("ltr %w0" : : "r"((uint16)GDT_TSS));
    
    //store in percpu
    percpu_t *cpu = percpu_get_by_index(cpu_index);
    if (cpu) {
        cpu->tss = (struct tss *)ap_tss;
        cpu->gdt = ap_gdt;
        cpu->ist_stack = ap_ist;
    }
    
    printf("[gdt] AP %u GDT/TSS initialized\n", cpu_index);
}

void tss_set_rsp0(uint64 rsp) {
    tss.rsp0 = rsp;
}

tss_t *tss_get(void) {
    return &tss;
}

//set kernel stack for ring 3 -> ring 0 transitions
void arch_set_kernel_stack(void *stack_top) {
    //get this CPU's TSS
    percpu_t *cpu = percpu_get();
    if (cpu && cpu->tss) {
        ((tss_t *)cpu->tss)->rsp0 = (uint64)stack_top;
    } else {
        tss_set_rsp0((uint64)stack_top);
    }
    percpu_set_kernel_stack(stack_top);
}

