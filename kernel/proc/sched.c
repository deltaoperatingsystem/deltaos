#include <proc/sched.h>
#include <proc/process.h>
#include <arch/cpu.h>
#include <arch/context.h>
#include <arch/mmu.h>
#include <arch/amd64/int/tss.h>
#include <lib/io.h>
#include <drivers/serial.h>

#define KERNEL_STACK_SIZE 16384  //16KB

//simple round-robin queue
static thread_t *run_queue_head = NULL;
static thread_t *run_queue_tail = NULL;
static uint32 tick_count = 0;
static uint32 time_slice = 10;  //switch every 10 ticks

//dead thread list - threads waiting to have their resources freed
static thread_t *dead_list_head = NULL;

extern void thread_set_current(thread_t *thread);
extern void process_set_current(process_t *proc);
extern void arch_context_load(arch_context_t *ctx);

//reap dead threads (free their resources)
static void reap_dead_threads(void) {
    while (dead_list_head) {
        thread_t *dead = dead_list_head;
        dead_list_head = dead->sched_next;
        thread_destroy(dead);
    }
}

void sched_init(void) {
    run_queue_head = NULL;
    run_queue_tail = NULL;
    dead_list_head = NULL;
    tick_count = 0;
}

void sched_add(thread_t *thread) {
    if (!thread) return;
    
    thread->sched_next = NULL;
    
    if (!run_queue_tail) {
        run_queue_head = thread;
        run_queue_tail = thread;
    } else {
        run_queue_tail->sched_next = thread;
        run_queue_tail = thread;
    }
    
    thread->state = THREAD_STATE_READY;
}

void sched_remove(thread_t *thread) {
    if (!thread) return;
    
    thread_t **tp = &run_queue_head;
    while (*tp) {
        if (*tp == thread) {
            *tp = thread->sched_next;
            if (run_queue_tail == thread) {
                run_queue_tail = NULL;
                //find new tail
                thread_t *t = run_queue_head;
                while (t && t->sched_next) t = t->sched_next;
                run_queue_tail = t;
            }
            thread->sched_next = NULL;
            return;
        }
        tp = &(*tp)->sched_next;
    }
}

//pick next thread and switch to it
//when called from ISR, ISR has already saved context and will restore new threads context
//when called from sched_yield we are in kernel context and can use arch_context_switch
static void schedule(void) {
    thread_t *current = thread_current();
    thread_t *next = NULL;
    
    if (!run_queue_head) {
        //no threads to run
        return;
    }
    
    //round-robin: pick head of queue
    next = run_queue_head;
    
    if (current && current->state == THREAD_STATE_RUNNING) {
        //move current to end of queue
        current->state = THREAD_STATE_READY;
        sched_remove(current);
        sched_add(current);
    }
    
    if (next == current) {
        //same thread, nothing to do
        return;
    }
    
    //switch to next thread
    sched_remove(next);
    next->state = THREAD_STATE_RUNNING;
    thread_set_current(next);
    process_set_current(next->process);
    
    //switch address space if different process has user pagemap
    process_t *next_proc = next->process;
    process_t *curr_proc = current ? current->process : NULL;
    
    if (next_proc && next_proc->pagemap) {
        //switching to userspace process - load its address space
        mmu_switch((pagemap_t *)next_proc->pagemap);
    } else if (curr_proc && curr_proc->pagemap) {
        //switching from user to kernel - reload kernel pagemap
        mmu_switch(mmu_get_kernel_pagemap());
    }
    
    //set TSS rsp0 for ring 3 -> ring 0 transitions
    uint64 kernel_stack_top = (uint64)next->kernel_stack + next->kernel_stack_size;
    tss_set_rsp0(kernel_stack_top);
}

void sched_yield(void) {
    schedule();
}

void sched_exit(void) {
    thread_t *current = thread_current();
    if (!current) return;
    
    //mark as dead and add to dead list for cleanup
    current->state = THREAD_STATE_DEAD;
    current->sched_next = dead_list_head;
    dead_list_head = current;
    
    //clear current thread
    thread_set_current(NULL);
    
    //schedule next thread
    schedule();
    
    //should never reach here
    for(;;) __asm__ volatile("hlt");
}

void sched_tick(void) {
    //reap dead threads
    reap_dead_threads();
    
    tick_count++;
    if (tick_count >= time_slice) {
        tick_count = 0;
        schedule();
    }
}

void sched_start(void) {
    if (!run_queue_head) {
        printf("[sched] no threads to run!\n");
        return;
    }
    
    thread_t *first = run_queue_head;
    sched_remove(first);
    first->state = THREAD_STATE_RUNNING;
    thread_set_current(first);
    process_set_current(first->process);
    
    //switch address space if first thread has user pagemap
    if (first->process && first->process->pagemap) {
        mmu_switch((pagemap_t *)first->process->pagemap);
    }
    
    //set TSS rsp0 for ring 3 -> ring 0 transitions
    uint64 kernel_stack_top = (uint64)first->kernel_stack + first->kernel_stack_size;
    tss_set_rsp0(kernel_stack_top);
    
    printf("[sched] starting scheduler with thread %llu\n", first->tid);
    
    //check if this is a usermode thread (cs has RPL=3)
    if ((first->context.cs & 3) == 3) {
        //enter usermode for the first time
        arch_enter_usermode(&first->context);
    } else {
        //kernel thread so just jump to entry point
        void (*entry)(void *) = (void (*)(void *))first->context.rip;
        void *arg = (void *)first->context.rdi;
        entry(arg);
    }
}
