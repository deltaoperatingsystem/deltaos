#include <proc/sched.h>
#include <proc/process.h>
#include <arch/cpu.h>
#include <arch/context.h>
#include <arch/interrupts.h>
#include <arch/mmu.h>
#include <lib/io.h>
#include <drivers/serial.h>
#include <lib/spinlock.h>
#include <arch/percpu.h>

#define KERNEL_STACK_SIZE 16384  //16KB

//switch every 10 ticks
static uint32 time_slice = 10;

//dead thread list - threads waiting to have their resources freed
static thread_t *dead_list_head = NULL;
static spinlock_irq_t dead_lock = SPINLOCK_INIT;

extern void process_set_current(process_t *proc);

//reap dead threads (free their resources)
void sched_reap(void) {
    spinlock_irq_acquire(&dead_lock);
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
    
    spinlock_irq_release(&dead_lock);
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
    dead_list_head = NULL;
    percpu_t *pc = percpu_get();
    pc->tick_count = 0;
    pc->run_queue_head = NULL;
    pc->run_queue_tail = NULL;
    pc->idle_thread = NULL;
    spinlock_irq_init(&pc->sched_lock);
    
    //create idle thread attached to kernel process
    process_t *kernel = process_get_kernel();
    pc->idle_thread = thread_create(kernel, idle_thread_entry, NULL);
    if (!pc->idle_thread) {
        printf("[sched] CRITICAL: failed to create idle thread for BSP!\n");
        for(;;) arch_halt();
    }
    pc->idle_thread->state = THREAD_STATE_READY;
}

void sched_init_ap(void) {
    percpu_t *pc = percpu_get();
    pc->tick_count = 0;
    pc->run_queue_head = NULL;
    pc->run_queue_tail = NULL;
    pc->idle_thread = NULL;
    spinlock_irq_init(&pc->sched_lock);

    //create unique idle thread for this AP
    process_t *kernel = process_get_kernel();
    pc->idle_thread = thread_create(kernel, idle_thread_entry, NULL);
    if (!pc->idle_thread) {
        printf("[sched] CRITICAL: failed to create idle thread for AP %u!\n", pc->cpu_index);
        for(;;) arch_halt();
    }
    pc->idle_thread->state = THREAD_STATE_READY;
}

void sched_add(thread_t *thread) {
    if (!thread) return;
    percpu_t *pc = percpu_get();
    if (thread == pc->idle_thread) return; //don't add idle to queue
    
    spinlock_irq_acquire(&pc->sched_lock);
    
    thread->sched_next = NULL;
    
    if (!pc->run_queue_tail) {
        pc->run_queue_head = thread;
        pc->run_queue_tail = thread;
    } else {
        pc->run_queue_tail->sched_next = thread;
        pc->run_queue_tail = thread;
    }
    
    thread->state = THREAD_STATE_READY;
    
    spinlock_irq_release(&pc->sched_lock);
}

void sched_remove(thread_t *thread) {
    if (!thread) return;
    percpu_t *pc = percpu_get();
    if (thread == pc->idle_thread) return;  //idle never in queue
    
    spinlock_irq_acquire(&pc->sched_lock);
    
    thread_t **tp = &pc->run_queue_head;
    while (*tp) {
        if (*tp == thread) {
            *tp = thread->sched_next;
            if (pc->run_queue_tail == thread) {
                //find new tail
                thread_t *t = pc->run_queue_head;
                while (t && t->sched_next) t = t->sched_next;
                pc->run_queue_tail = t;
            }
            thread->sched_next = NULL;
            break;
        }
        tp = &(*tp)->sched_next;
    }
    
    spinlock_irq_release(&pc->sched_lock);
}

//pick next thread - returns idle thread if no other threads
static thread_t *pick_next(void) {
    percpu_t *pc = percpu_get();
    if (pc->run_queue_head) {
        return pc->run_queue_head;
    }
    return pc->idle_thread;
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
    percpu_t *pc = percpu_get();
    
    //reap any dead threads before scheduling
    sched_reap();
    
    spinlock_irq_acquire(&pc->sched_lock);
    
    thread_t *next = pc->run_queue_head ? pc->run_queue_head : pc->idle_thread;
    
    if (!next || next == current) {
        spinlock_irq_release(&pc->sched_lock);
        return;
    }

    //if current is runnable, move it back to run queue
    if (current && current->state == THREAD_STATE_RUNNING && current != pc->idle_thread) {
        current->state = THREAD_STATE_READY;
        current->sched_next = NULL;
        if (!pc->run_queue_tail) {
            pc->run_queue_head = current;
            pc->run_queue_tail = current;
        } else {
            pc->run_queue_tail->sched_next = current;
            pc->run_queue_tail = current;
        }
    }
    
    //remove next from run queue and mark as running
    if (next != pc->idle_thread) {
        thread_t **tp = &pc->run_queue_head;
        while (*tp) {
            if (*tp == next) {
                *tp = next->sched_next;
                if (pc->run_queue_tail == next) {
                    thread_t *t = pc->run_queue_head;
                    while (t && t->sched_next) t = t->sched_next;
                    pc->run_queue_tail = t;
                }
                next->sched_next = NULL;
                break;
            }
            tp = &(*tp)->sched_next;
        }
    }
    next->state = THREAD_STATE_RUNNING;
    
    //activate next thread
    sched_activate(next);
    
    //must release lock before context switch
    spinlock_irq_release(&pc->sched_lock);
    
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
    spinlock_irq_acquire(&dead_lock);
    
    thread_t *current = thread_current();
    if (!current) {
        spinlock_irq_release(&dead_lock);
        return;
    }
    
    //mark as dead and add to dead list for cleanup
    //it's already not in the run queue since it's the running thread
    current->state = THREAD_STATE_DEAD;
    current->sched_next = dead_list_head;
    dead_list_head = current;

    spinlock_irq_release(&dead_lock);
    
    //schedule next thread (will be idle if no others)
    //this will NOT return to current - we switch away and never come back
    schedule();
    
    //should never reach here
    for(;;) arch_halt();
}

//ISR-safe preemption 
//only updates scheduler state no context switch
static void sched_preempt(void) {
    percpu_t *pc = percpu_get();
    spinlock_irq_acquire(&pc->sched_lock);
    
    thread_t *current = thread_current();
    thread_t *next = pc->run_queue_head ? pc->run_queue_head : pc->idle_thread;
    
    if (!next || next == current) {
        spinlock_irq_release(&pc->sched_lock);
        return;
    }

    if (current && current->state == THREAD_STATE_RUNNING && current != pc->idle_thread) {
        current->state = THREAD_STATE_READY;
        current->sched_next = NULL;
        if (!pc->run_queue_tail) {
            pc->run_queue_head = current;
            pc->run_queue_tail = current;
        } else {
            pc->run_queue_tail->sched_next = current;
            pc->run_queue_tail = current;
        }
    }
    
    if (next != pc->idle_thread) {
        thread_t **tp = &pc->run_queue_head;
        while (*tp) {
            if (*tp == next) {
                *tp = next->sched_next;
                if (pc->run_queue_tail == next) {
                    thread_t *t = pc->run_queue_head;
                    while (t && t->sched_next) t = t->sched_next;
                    pc->run_queue_tail = t;
                }
                next->sched_next = NULL;
                break;
            }
            tp = &(*tp)->sched_next;
        }
    }
    next->state = THREAD_STATE_RUNNING;
    
    sched_activate(next);
    
    spinlock_irq_release(&pc->sched_lock);
}

void sched_tick(int from_usermode) {
    //reap dead threads
    sched_reap();
    
    percpu_t *pc = percpu_get();
    pc->tick_count++;
    if (from_usermode && pc->tick_count >= time_slice) {
        pc->tick_count = 0;
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
