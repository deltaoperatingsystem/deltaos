#ifndef ARCH_AMD64_PERCPU_H
#define ARCH_AMD64_PERCPU_H

#include <arch/types.h>

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
#define PERCPU_SELF         24

typedef struct percpu {
    uint64 kernel_rsp;      // 0: kernel stack pointer (top of kernel stack)
    uint64 user_rsp;        // 8: saved user stack pointer during syscall
    void *current_thread;   //16: current thread pointer
    struct percpu *self;    //24: pointer to self (for accessing via GS)
} percpu_t;

//get pointer to current CPU's per-CPU data
percpu_t *percpu_get(void);

//initialize per-CPU data for the boot CPU
void percpu_init(void);

//set kernel stack for syscalls (called on context switch to user thread)
void percpu_set_kernel_stack(void *stack_top);

#endif
