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
#define PERCPU_PREV         32
#define PERCPU_SELF         40
#define PERCPU_TSS          48
#define PERCPU_GDT          56
#define PERCPU_IST_STACK    64
#define PERCPU_RUN_Q_HEAD   72
#define PERCPU_RUN_Q_TAIL   80
#define PERCPU_IDLE         88
#define PERCPU_CPU_INDEX    96
#define PERCPU_APIC_ID      100
#define PERCPU_STARTED      104
#define PERCPU_TICKS        108

//forward declarations
struct tss;
struct gdt_entry;

typedef struct percpu {
    //0-47: core pointers
    uint64 kernel_rsp;      // 0: kernel stack pointer (top of kernel stack)
    uint64 user_rsp;        // 8: saved user stack pointer during syscall
    void *current_thread;   //16: current thread pointer
    void *current_process;  //24: current process pointer
    void *prev_thread;      //32: thread we just switched from (to be cleared)
    struct percpu *self;    //40: pointer to self (for accessing via GS)

    //48-71: arch pointers
    struct tss *tss;        //48: pointer to this CPU's TSS
    struct gdt_entry *gdt;  //56: ...
    void *ist_stack;        //64: ...

    //72-95: scheduler pointers
    struct thread *run_queue_head; //72
    struct thread *run_queue_tail; //80
    struct thread *idle_thread;    //88

    //96-111: counters and IDs
    uint32 cpu_index;       //96
    uint32 apic_id;         //100
    volatile uint32 started;//104
    uint32 tick_count;      //108
    
    //112+: synchronisation
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

