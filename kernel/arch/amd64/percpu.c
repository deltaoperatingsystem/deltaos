#include <arch/amd64/percpu.h>
#include <arch/amd64/acpi/acpi.h>
#include <arch/io.h>
#include <lib/io.h>
#include <lib/string.h>
#include <mm/kheap.h>

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

//per-CPU data array (one per CPU)
static percpu_t percpu_array[MAX_CPUS];
static uint32 num_cpus = 0;

uint32 arch_cpu_index(void) {
    percpu_t *cpu = percpu_get();
    return cpu ? cpu->cpu_index : 0;
}

percpu_t *percpu_get(void) {
    percpu_t *cpu;
    __asm__ volatile ("mov %%gs:%c1, %0" : "=r"(cpu) : "i"(PERCPU_SELF));
    return cpu;
}

percpu_t *percpu_get_by_index(uint32 index) {
    if (index >= MAX_CPUS) return NULL;
    return &percpu_array[index];
}

uint32 percpu_cpu_count(void) {
    return num_cpus;
}

void percpu_init(void) {
    //BSP is always CPU 0
    percpu_t *bsp = &percpu_array[0];
    
    memset(bsp, 0, sizeof(percpu_t));
    bsp->kernel_rsp = 0;
    bsp->user_rsp = 0;
    bsp->current_thread = NULL;
    bsp->current_process = NULL;
    bsp->self = bsp;
    bsp->cpu_index = 0;
    bsp->apic_id = 0;
    bsp->started = 1;
    bsp->tick_count = 0;
    
    bsp->run_queue_head = NULL;
    bsp->run_queue_tail = NULL;
    bsp->idle_thread = NULL;
    spinlock_irq_init(&bsp->sched_lock);

    bsp->tss = NULL;
    bsp->gdt = NULL;
    bsp->ist_stack = NULL;
    
    wrmsr(IA32_KERNEL_GS_BASE, (uint64)bsp);
    wrmsr(IA32_GS_BASE, (uint64)bsp);
    
    //get CPU count from ACPI
    num_cpus = acpi_cpu_count > 0 ? acpi_cpu_count : 1;
    if (num_cpus > MAX_CPUS) num_cpus = MAX_CPUS;
    
    printf("[percpu] initialized for BSP, %u CPUs detected\n", num_cpus);
}

void percpu_init_ap(uint32 cpu_index, uint32 apic_id) {
    if (cpu_index >= MAX_CPUS) return;
    
    percpu_t *ap = &percpu_array[cpu_index];
    
    memset(ap, 0, sizeof(percpu_t));
    ap->kernel_rsp = 0;
    ap->user_rsp = 0;
    ap->current_thread = NULL;
    ap->current_process = NULL;
    ap->self = ap;
    ap->cpu_index = cpu_index;
    ap->apic_id = apic_id;
    ap->started = 0;
    ap->tick_count = 0;

    ap->run_queue_head = NULL;
    ap->run_queue_tail = NULL;
    ap->idle_thread = NULL;
    spinlock_irq_init(&ap->sched_lock);

    ap->tss = NULL;
    ap->gdt = NULL;
    ap->ist_stack = NULL;
    
    //set GS bases for this AP
    wrmsr(IA32_KERNEL_GS_BASE, (uint64)ap);
    wrmsr(IA32_GS_BASE, (uint64)ap);
}

void percpu_set_kernel_stack(void *stack_top) {
    percpu_t *cpu = percpu_get();
    cpu->kernel_rsp = (uint64)stack_top;
}

