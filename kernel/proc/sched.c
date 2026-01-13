#include <proc/sched.h>
#include <proc/process.h>
#include <arch/cpu.h>
#include <arch/context.h>
#include <arch/interrupts.h>
#include <arch/mmu.h>
#include <lib/io.h>
#include <drivers/serial.h>

#define KERNEL_STACK_SIZE 16384  //16KB

//simple round-robin queue
static thread_t *run_queue_head = NULL;
static thread_t *run_queue_tail = NULL;
static uint32 tick_count = 0;
static uint32 time_slice = 10; //switch every 10 ticks

//dead thread list - threads waiting to have their resources freed
static thread_t *dead_list_head = NULL;

//idle thread - always runnable runs when no other threads available
static thread_t *idle_thread = NULL;

extern void thread_set_current(thread_t *thread);
extern void process_set_current(process_t *proc);

//reap dead threads (free their resources)
static void reap_dead_threads(void) {
    while (dead_list_head) {
        thread_t *dead = dead_list_head;
        dead_list_head = dead->sched_next;
        thread_destroy(dead);
    }
}

//idle thread entry - just halts forever
static void idle_thread_entry(void *arg) {
    (void)arg;
    
    for (;;) {
        arch_halt();
    }
}

void sched_init(void) {
    run_queue_head = NULL;
    run_queue_tail = NULL;
    dead_list_head = NULL;
    tick_count = 0;
    
    //create idle thread attached to kernel process
    process_t *kernel = process_get_kernel();
    idle_thread = thread_create(kernel, idle_thread_entry, NULL);
    if (idle_thread) {
        idle_thread->state = THREAD_STATE_READY;
    }
}

void sched_add(thread_t *thread) {
    if (!thread) return;
    if (thread == idle_thread) return; //don't add idle to queue
    
    //save interrupt state and disable - we may be called from IRQ context
    irq_state_t flags = arch_irq_save();
    
    thread->sched_next = NULL;
    
    if (!run_queue_tail) {
        run_queue_head = thread;
        run_queue_tail = thread;
    } else {
        run_queue_tail->sched_next = thread;
        run_queue_tail = thread;
    }
    
    thread->state = THREAD_STATE_READY;
    
    //restore interrupt state
    arch_irq_restore(flags);
}

void sched_remove(thread_t *thread) {
    if (!thread) return;
    if (thread == idle_thread) return;  //idle never in queue
    
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

//pick next thread - returns idle thread if no other threads
static thread_t *pick_next(void) {
    if (run_queue_head) {
        return run_queue_head;
    }
    return idle_thread;
}

//pick next thread and switch to it
static void schedule(void) {
    thread_t *current = thread_current();
    thread_t *next = pick_next();
    
    if (!next) {
        //no idle thread that shouldn't happen
        return;
    }
    
    //don't switch to idle if current thread is still runnable
    //this happens during yield when there's only one user thread
    if (next == idle_thread && current && current->state == THREAD_STATE_RUNNING) {
        return;
    }
    
    if (current && current->state == THREAD_STATE_RUNNING && current != idle_thread) {
        //move current to end of queue
        current->state = THREAD_STATE_READY;
        sched_remove(current);
        sched_add(current);
    }
    
    if (next == current) {
        //same thread so nothing to do
        return;
    }
    
    //switch to next thread
    if (next != idle_thread) {
        sched_remove(next);
    }
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
    
    //set kernel stack for ring 3 -> ring 0 transitions
    void *kernel_stack_top = (char *)next->kernel_stack + next->kernel_stack_size;
    arch_set_kernel_stack(kernel_stack_top);
    
    //switch CPU context
    if (current) {
        arch_context_switch(&current->context, &next->context);
    } else {
        arch_context_load(&next->context);
    }
}

void sched_yield(void) {
    schedule();
}

void sched_exit(void) {
    //disable interrupts - critical section soooo can't have timer fire during exit
    arch_interrupts_disable();
    
    thread_t *current = thread_current();
    if (!current) {
        arch_interrupts_enable();
        return;
    }
    
    //mark as dead and add to dead list for cleanup
    current->state = THREAD_STATE_DEAD;
    current->sched_next = dead_list_head;
    dead_list_head = current;
    
    //clear current thread
    thread_set_current(NULL);
    
    //schedule next thread (will be idle if no others)
    schedule();
    
    //schedule() set up the next thread - we need to actually jump to it
    //interrupts will be re-enabled when we iret/sysret to the next thread
    thread_t *next = thread_current();
    if (next) {
        if ((next->context.cs & 3) == 3) {
            //usermode thread
            arch_enter_usermode(&next->context);
        } else {
            //kernel thread - load its context
            arch_context_load(&next->context);
        }
    }
    
    //should never reach here
    for(;;) arch_halt();
}

//ISR-safe preemption 
//only updates scheduler state no context swithc
//the ISR will restore the new thread's context via its normal iretq path
static void sched_preempt(void) {
    thread_t *current = thread_current();
    
    //only preempt if there's an actual runnable thread in the queue
    //we can't switch to idle via ISR path because its context wasn't
    //set up for iretq restoration (it was set up for cooperative switching)
    if (!run_queue_head) return;  //nothing to preempt to
    
    thread_t *next = run_queue_head;
    
    //if current thread is still running, move to end of queue
    if (current && current->state == THREAD_STATE_RUNNING && current != idle_thread) {
        current->state = THREAD_STATE_READY;
        sched_remove(current);
        sched_add(current);
    }
    
    if (next == current) return;  //same thread, nothing to do
    
    //update scheduler state - the ISR saved current's context already
    //and will restore next's context via iretq
    if (next != idle_thread) {
        sched_remove(next);
    }
    next->state = THREAD_STATE_RUNNING;
    thread_set_current(next);
    process_set_current(next->process);
    
    //switch address space if needed
    process_t *next_proc = next->process;
    process_t *curr_proc = current ? current->process : NULL;
    
    if (next_proc && next_proc->pagemap) {
        mmu_switch((pagemap_t *)next_proc->pagemap);
    } else if (curr_proc && curr_proc->pagemap) {
        mmu_switch(mmu_get_kernel_pagemap());
    }
    
    //set kernel stack for ring 3 -> ring 0 transitions
    void *kernel_stack_top = (char *)next->kernel_stack + next->kernel_stack_size;
    arch_set_kernel_stack(kernel_stack_top);
}

void sched_tick(int from_usermode) {
    //reap dead threads
    reap_dead_threads();
    
    tick_count++;
    if (tick_count >= time_slice) {
        tick_count = 0;
        //only preempt when interrupted from usermode
        //kernel-mode preemption is not safe (thread may be in syscall)
        if (from_usermode) {
            sched_preempt();  //ISR-safe: only updates state, lets ISR do context switch
        }
    }
}

void sched_start(void) {
    thread_t *first = pick_next();
    if (!first) {
        printf("[sched] no threads to run!\n");
        return;
    }
    
    sched_remove(first);
    first->state = THREAD_STATE_RUNNING;
    thread_set_current(first);
    process_set_current(first->process);
    
    //switch address space if first thread has user pagemap
    if (first->process && first->process->pagemap) {
        mmu_switch((pagemap_t *)first->process->pagemap);
    }
    
    //set kernel stack for ring 3 -> ring 0 transitions
    void *kernel_stack_top = (char *)first->kernel_stack + first->kernel_stack_size;
    arch_set_kernel_stack(kernel_stack_top);
    
    printf("[sched] starting scheduler with thread %llu\n", first->tid);
    
    //check if this is a usermode thread (cs has RPL=3)
    if ((first->context.cs & 3) == 3) {
        //enter usermode for the first time
        printf("[sched] entering usermode: rip=0x%lX, rsp=0x%lX\n", 
               first->context.rip, first->context.rsp);
        printf("[sched] pagemap=0x%lX, cs=0x%lX, ss=0x%lX\n",
               first->process->pagemap ? ((pagemap_t*)first->process->pagemap)->top_level : 0,
               first->context.cs, first->context.ss);
        
        //verify the entry point is mapped
        uintptr phys = mmu_virt_to_phys((pagemap_t*)first->process->pagemap, first->context.rip);
        printf("[sched] virt 0x%lX -> phys 0x%lX\n", first->context.rip, phys);
        
        //verify stack is mapped
        uintptr stack_phys = mmu_virt_to_phys((pagemap_t*)first->process->pagemap, first->context.rsp);
        printf("[sched] stack 0x%lX -> phys 0x%lX\n", first->context.rsp, stack_phys);
        
        arch_enter_usermode(&first->context);
    } else {
        //kernel thread so just jump to entry point
        void (*entry)(void *) = (void (*)(void *))first->context.rip;
        void *arg = (void *)first->context.rdi;
        entry(arg);
    }
}
