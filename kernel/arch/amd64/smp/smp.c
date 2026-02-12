#include <arch/amd64/smp/smp.h>
#include <arch/smp.h>
#include <arch/amd64/percpu.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/gdt.h>
#include <arch/amd64/int/idt.h>
#include <arch/amd64/acpi/acpi.h>
#include <arch/amd64/mmu.h>
#include <arch/amd64/cpu.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>
#include <proc/sched.h>
#include <lib/io.h>
#include <lib/string.h>

//trampoline is assembled separately and linked
extern uint8 _trampoline_start[];
extern uint8 _trampoline_end[];

extern void syscall_init(void);
extern void enable_sse(void);

//trampoline data structure - shared between BSP and APs
//located at a fixed address following the trampoline code
typedef struct {
    uint64 stack_top;       //0x00: AP stack pointer
    uint64 cr3;             //0x08: page table to use
    uint64 entry;           //0x10: C entry point
    uint32 cpu_index;       //0x18: logical CPU index
    uint32 apic_id;         //0x1C: APIC ID
    uint64 gdt_ptr;         //0x20: GDT pointer address
    uint64 idt_ptr;         //0x28: IDT pointer address
} __attribute__((packed)) trampoline_data_t;

//trampoline is placed at this physical address (must be < 1MB, page-aligned)
#define TRAMPOLINE_ADDR     0x8000
#define TRAMPOLINE_DATA     (TRAMPOLINE_ADDR + 0x1000)  //data follows code

//AP stack size
#define AP_STACK_SIZE       16384

//atomic counter for started APs
static volatile uint32 ap_started_count = 0;

//delay using APIC timer or busy loop
static void delay_us(uint32 us) {
    //simple busy loop - not accurate but sufficient for INIT-SIPI timing
    //approximately calibrated for modern CPUs
    volatile uint64 count = us * 1000;
    while (count--) {
        arch_pause();
    }
}

//send INIT IPI to an AP
static void apic_send_init(uint8 apic_id) {
    apic_wait_icr_idle();
    apic_write(APIC_ICR_HIGH, (uint32)apic_id << 24);
    //INIT IPI: delivery mode = 5 (INIT), level = 1 (assert)
    apic_write(APIC_ICR_LOW, (5 << 8) | (1 << 14));
    
    //wait for delivery
    apic_wait_icr_idle();
}

//send SIPI to an AP
static void apic_send_sipi(uint8 apic_id, uint8 vector) {
    apic_wait_icr_idle();
    apic_write(APIC_ICR_HIGH, (uint32)apic_id << 24);
    //SIPI: delivery mode = 6 (Startup), vector = page number of startup code
    apic_write(APIC_ICR_LOW, vector | (6 << 8));
    
    //wait for delivery
    apic_wait_icr_idle();
}

//start a single AP
static bool start_ap(uint32 cpu_index, uint8 apic_id) {
    printf("[smp] Starting AP %u (APIC ID %u)...\n", cpu_index, apic_id);
    
    //allocate stack for this AP
    void *stack = kmalloc(AP_STACK_SIZE);
    if (!stack) {
        printf("[smp] ERR: Failed to allocate AP stack\n");
        return false;
    }
    
    //set up trampoline data for this AP
    trampoline_data_t *data = (trampoline_data_t *)P2V(TRAMPOLINE_DATA);
    data->stack_top = (uint64)stack + AP_STACK_SIZE;
    data->cr3 = mmu_get_kernel_cr3();
    data->entry = (uint64)ap_entry;
    data->cpu_index = cpu_index;
    data->apic_id = apic_id;
    data->gdt_ptr = 0;  //AP will set up its own GDT
    data->idt_ptr = (uint64)idt_get_ptr();
    
    printf("[smp] Data @ 0x%lx: stack=0x%lx cr3=0x%lx entry=0x%lx cpu=%u\n",
           (unsigned long)TRAMPOLINE_DATA, data->stack_top, data->cr3, 
           data->entry, data->cpu_index);
    
    //memory barrier to ensure data is visible
    arch_wmb();
    
    uint32 prev_count = ap_started_count;
    
    //INIT-SIPI-SIPI sequence
    apic_send_init(apic_id);
    delay_us(10000);  //10ms delay after INIT
    
    //de-assert INIT
    apic_wait_icr_idle();
    apic_write(APIC_ICR_HIGH, (uint32)apic_id << 24);
    apic_write(APIC_ICR_LOW, (5 << 8) | (0 << 14));  //INIT, de-assert
    
    delay_us(200);  //200us delay
    
    //first SIPI
    apic_send_sipi(apic_id, TRAMPOLINE_ADDR >> 12);
    delay_us(200);  //200us delay
    
    //second SIPI (some processors need it)
    apic_send_sipi(apic_id, TRAMPOLINE_ADDR >> 12);
    
    //wait for AP to start (with timeout)
    for (int i = 0; i < 1000; i++) {
        if (ap_started_count > prev_count) {
            printf("[smp] AP %u online\n", cpu_index);
            return true;
        }
        delay_us(1000);  //1ms per iteration 1s total timeout
    }
    
    printf("[smp] ERR: AP %u failed to start (timeout)\n", cpu_index);
    kfree(stack);
    return false;
}

void smp_init(void) {
    printf("[smp] Initializing SMP...\n");
    
    uint32 bsp_apic_id = apic_get_id();
    printf("[smp] BSP APIC ID: %u\n", bsp_apic_id);
    
    //update BSP's percpu with correct APIC ID
    percpu_t *bsp = percpu_get_by_index(0);
    if (bsp) bsp->apic_id = bsp_apic_id;
    
    uint32 cpu_count = acpi_cpu_count;
    if (cpu_count <= 1) {
        printf("[smp] Single CPU system, SMP init complete\n");
        return;
    }
    
    //copy trampoline code to low memory
    size trampoline_size = (size)(_trampoline_end - _trampoline_start);
    printf("[smp] Trampoline size: %lu bytes\n", (unsigned long)trampoline_size);
    if (trampoline_size > 0x1000) {
        printf("[smp] ERR: Trampoline too large (%lu bytes)\n", (unsigned long)trampoline_size);
        return;
    }
    
    //map trampoline in HHDM for BSP access
    vmm_kernel_map((uintptr)P2V(TRAMPOLINE_ADDR), TRAMPOLINE_ADDR, 2, MMU_FLAG_WRITE | MMU_FLAG_EXEC);
    
    //identity map low memory (phys=virt) for AP startup
    //map the first 1MB to ensure we don't hit any unmapped regions during transition
    pagemap_t *kmap = mmu_get_kernel_pagemap();
    mmu_map_range(kmap, 0, 0, 256, MMU_FLAG_WRITE | MMU_FLAG_EXEC);
    printf("[smp] Identity mapped first 1MB\n");
    
    memcpy((void *)P2V(TRAMPOLINE_ADDR), _trampoline_start, trampoline_size);
    arch_wmb();
    
    //start each AP (skip BSP at index 0)
    uint32 started = 1;  //BSP counts as 1
    for (uint32 i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        uint8 apic_id = acpi_cpu_ids[i];
        
        //skip BSP
        if (apic_id == bsp_apic_id) continue;
        
        if (start_ap(started, apic_id)) {
            started++;
        }
    }
    
    printf("[smp] %u CPUs online\n", started);
}

uint32 smp_cpu_count(void) {
    return ap_started_count + 1;  //+1 for BSP
}

//MI wrapper for cpu count
uint32 arch_cpu_count(void) {
    return smp_cpu_count();
}

bool smp_ap_started(uint32 cpu_index) {
    if (cpu_index >= MAX_CPUS) return false;
    percpu_t *cpu = percpu_get_by_index(cpu_index);
    return cpu && cpu->started;
}

//AP entry point - called after trampoline brings AP to long mode
void ap_entry(uint32 cpu_index) {
    //get our data from the percpu array
    uint8 apic_id = acpi_cpu_ids[cpu_index];
    
    //initialize per-CPU data
    percpu_init_ap(cpu_index, apic_id);
    
    //initialize this AP's GDT and TSS
    gdt_init_ap(cpu_index);
    
    //enable SSE for this AP
    enable_sse();
    
    //initialize this AP's scheduler
    sched_init_ap();
    
    //load IDT (same as BSP)
    idt_load();
    
    //initialize syscall handling for this AP
    syscall_init();
    
    //enable local APIC for this AP
    //the APIC is already enabled via MSR, just need to set spurious vector
    apic_write(APIC_SPURIOUS, (1 << 8) | 0xFF);
    
    //signal that we're online
    __sync_fetch_and_add(&ap_started_count, 1);
    
    percpu_t *cpu = percpu_get();
    cpu->started = 1;
    arch_wmb();
    
    printf("[smp] AP %u: initialized (APIC ID %u)\n", cpu_index, apic_id);
    
    //start the scheduler on this AP
    sched_start();
}

void arch_smp_send_resched(uint32 cpu_index) {
    percpu_t *cpu = percpu_get_by_index(cpu_index);
    if (!cpu || !cpu->started) return;
    
    apic_send_ipi(cpu->apic_id, IPI_RESCHEDULE);
}
