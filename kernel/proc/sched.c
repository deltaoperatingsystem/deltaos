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

extern void process_set_current(process_t *proc);

//reap dead threads (free their resources)
void sched_reap(void) {
    irq_state_t flags = arch_irq_save();
    thread_t *current = thread_current();
    
    thread_t *list = dead_list_head;
    dead_list_head = NULL;
    
    thread_t *keep_head = NULL;
    thread_t *keep_tail = NULL;
    
    while (list) {
        thread_t *dead = list;
        list = list->sched_next;
        
        if (dead == current) {
            //can't reap current thread yet (still using the stack)
            dead->sched_next = NULL;
            if (!keep_head) {
                keep_head = dead;
                keep_tail = dead;
            } else {
                keep_tail->sched_next = dead;
                keep_tail = dead;
            }
        } else {
            thread_destroy(dead);
        }
    }
    
    //put back anything we couldn't reap
    if (keep_head) {
        keep_tail->sched_next = dead_list_head;
        dead_list_head = keep_head;
    }
    
    arch_irq_restore(flags);
}

//idle thread entry - just halts forever
static void idle_thread_entry(void *arg) {
    (void)arg;
    
    for (;;) {
        arch_halt();
        sched_yield();
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
    if (!idle_thread) {
        printf("[sched] CRITICAL: failed to create idle thread!\n");
        kpanic(NULL, "FATAL: failed to create idle thread!\n");
    }
    idle_thread->state = THREAD_STATE_READY;
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
    
    irq_state_t flags = arch_irq_save();
    
    thread_t **tp = &run_queue_head;
    while (*tp) {
        if (*tp == thread) {
            *tp = thread->sched_next;
            if (run_queue_tail == thread) {
                //find new tail
                thread_t *t = run_queue_head;
                while (t && t->sched_next) t = t->sched_next;
                run_queue_tail = t;
            }
            thread->sched_next = NULL;
            arch_irq_restore(flags);
            return;
        }
        tp = &(*tp)->sched_next;
    }
    
    arch_irq_restore(flags);
}

//pick next thread - returns idle thread if no other threads
static thread_t *pick_next(void) {
    if (run_queue_head) {
        return run_queue_head;
    }
    return idle_thread;
}

//update queues for context switch
static thread_t *sched_pick_and_prepare(void) {
    thread_t *current = thread_current();
    thread_t *next = pick_next();
    
    if (!next || next == current) return NULL;

    //if current is runnable but switch to IDLE or we are yielding/preempting
    //move current back to run queue if it's still running
    if (current && current->state == THREAD_STATE_RUNNING && current != idle_thread) {
        current->state = THREAD_STATE_READY;
        sched_add(current);
    }
    
    //remove next from run queue and mark as running
    if (next != idle_thread) {
        sched_remove(next);
    }
    next->state = THREAD_STATE_RUNNING;
    
    return next;
}

//activate a thread (switch address space, stack and shit)
static void sched_activate(thread_t *next) {
    thread_set_current(next);
    process_set_current(next->process);
    
    //switch address space
    process_t *next_proc = next->process;
    if (next_proc && next_proc->pagemap) {
        mmu_switch((pagemap_t *)next_proc->pagemap);
    } else {
        mmu_switch(mmu_get_kernel_pagemap());
    }
    
    //set kernel stack for ring 3 -> ring 0 transitions
    void *kernel_stack_top = (char *)next->kernel_stack + next->kernel_stack_size;
    arch_set_kernel_stack(kernel_stack_top);
}

//pick next thread and switch to it
static void schedule(void) {
    thread_t *current = thread_current();
    
    irq_state_t flags = arch_irq_save();
    
    //reap any dead threads before scheduling
    sched_reap();
    
    thread_t *next = sched_pick_and_prepare();
    
    if (!next) {
        arch_irq_restore(flags);
        return;
    }
    
    //activate next thread
    sched_activate(next);
    
    //switch CPU context
    if (current) {
        arch_context_switch(&current->context, &next->context);
    } else {
        arch_context_load(&next->context);
    }
    
    arch_irq_restore(flags);
}

void sched_yield(void) {
    schedule();
}

void sched_exit(void) {
    irq_state_t flags = arch_irq_save();
    
    thread_t *current = thread_current();
    if (!current) {
        arch_irq_restore(flags);
        return;
    }
    
    //mark as dead and add to dead list for cleanup
    //it's already not in the run queue since it's the running thread
    current->state = THREAD_STATE_DEAD;
    current->sched_next = dead_list_head;
    dead_list_head = current;

    (void)flags;
    
    //schedule next thread (will be idle if no others)
    //this will NOT return to current - we switch away and never come back
    schedule();
    
    //should never reach here
    kpanic(NULL, "FATAL: Scheduler returned!\n");
}

//ISR-safe preemption 
//only updates scheduler state no context switch
//the ISR will restore the new thread's context via its normal iretq path
static void sched_preempt(void) {
    //already in ISR so interrupts are disabled
    thread_t *next = sched_pick_and_prepare();
    if (!next) return;
    
    //activate next thread (switch address space, stack, etc)
    sched_activate(next);
}

void sched_tick(int from_usermode) {
    //reap dead threads
    sched_reap();
    
    tick_count++;
    if (from_usermode && tick_count >= time_slice) {
        tick_count = 0;
        //preempt any thread when its slice is over
        sched_preempt();  //ISR-safe: only updates current_thread and sched state
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
    
    //check if this is a usermode thread (cs has RPL=3)
    if ((first->context.cs & 3) == 3) {
        arch_enter_usermode(&first->context);
    } else {
        //kernel thread so just jump to entry point
        void (*entry)(void *) = (void (*)(void *))first->context.rip;
        void *arg = (void *)first->context.rdi;
        entry(arg);
    }
}
