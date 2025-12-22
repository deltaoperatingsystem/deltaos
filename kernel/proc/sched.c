#include <proc/sched.h>
#include <proc/process.h>
#include <arch/cpu.h>
#include <lib/io.h>

//simple round-robin queue
static thread_t *run_queue_head = NULL;
static thread_t *run_queue_tail = NULL;
static uint32 tick_count = 0;
static uint32 time_slice = 10;  //switch every 10 ticks

extern void thread_set_current(thread_t *thread);
extern void process_set_current(process_t *proc);

void sched_init(void) {
    run_queue_head = NULL;
    run_queue_tail = NULL;
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
    
    //actual context switch would happen here
    //for now we just update the current pointers
}

void sched_yield(void) {
    schedule();
}

void sched_tick(void) {
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
    
    printf("[sched] starting scheduler with thread %llu\n", first->tid);
    
    //in a real implementation, we would jump to the thread's entry point
    //for now, this is a placeholder
}
