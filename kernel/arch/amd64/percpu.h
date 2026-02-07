#ifndef ARCH_AMD64_PERCPU_H
#define ARCH_AMD64_PERCPU_H

#include <arch/types.h>
#include <lib/spinlock.h>

struct thread;

//maximum number of CPUs supported (matches ACPI arrays)
#define MAX_CPUS 64

/*
 *per-CPU data structure for AMD64
 * 
 *accessed via GS segment after swapgs:
 *- kernel mode: GS points to this structure
 *- user mode: GS points to user-defined TLS
 * 
 *The syscall instruction uses swapgs to swap between user and kernel GS.
 */

//offsets for assembly access (must match struct layout)
#define PERCPU_KERNEL_RSP   0
#define PERCPU_USER_RSP     8
#define PERCPU_CURRENT      16
#define PERCPU_CURRENT_PROC 24
#define PERCPU_SELF         32
#define PERCPU_CPU_INDEX    40
#define PERCPU_APIC_ID      44
#define PERCPU_STARTED      48
#define PERCPU_TICKS        52

//forward declarations
struct tss;
struct gdt_entry;

typedef struct percpu {
    uint64 kernel_rsp;      // 0: kernel stack pointer (top of kernel stack)
    uint64 user_rsp;        // 8: saved user stack pointer during syscall
    void *current_thread;   //16: current thread pointer
    void *current_process;  //24: current process pointer
    struct percpu *self;    //32: pointer to self (for accessing via GS)
    uint32 cpu_index;       //40: logical CPU index (0 = BSP)
    uint32 apic_id;         //44: local APIC ID
    volatile uint32 started;//48: set to 1 when AP is fully initialized
    uint32 tick_count;      //52: per-CPU tick count for scheduler
    
    struct tss *tss;        //56: pointer to this CPU's TSS
    struct gdt_entry *gdt;  //64: ...
    void *ist_stack;        //72: ...

    //scheduler state
    struct thread *run_queue_head;
    struct thread *run_queue_tail;
    struct thread *idle_thread;
    spinlock_irq_t sched_lock;
} percpu_t;

//get pointer to current CPU's per-CPU data
percpu_t *percpu_get(void);

//get percpu by CPU index
percpu_t *percpu_get_by_index(uint32 index);

//initialize per-CPU data for the boot CPU
void percpu_init(void);

//initialize per-CPU data for an AP (called by AP after entering long mode)
void percpu_init_ap(uint32 cpu_index, uint32 apic_id);

//set kernel stack for syscalls (called on context switch to user thread)
void percpu_set_kernel_stack(void *stack_top);

//get number of CPUs detected
uint32 percpu_cpu_count(void);

#endif

