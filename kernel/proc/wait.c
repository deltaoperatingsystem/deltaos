#include <proc/wait.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <arch/interrupts.h>
#include <arch/cpu.h>
#include <lib/io.h>
#include <lib/spinlock.h>

void wait_queue_init(wait_queue_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
}

void thread_sleep(wait_queue_t *wq) {
    thread_t *current = thread_current();
    if (!current) return;
    
    irq_state_t flags = arch_irq_save();
    
    //mark as blocked
    current->state = THREAD_STATE_BLOCKED;
    
    //add to wait queue
    current->wait_next = NULL;
    if (wq->tail) {
        wq->tail->wait_next = current;
    } else {
        wq->head = current;
    }
    wq->tail = current;
    
    arch_irq_restore(flags);
    
    //wait until woken
    while (current->state == THREAD_STATE_BLOCKED) {
        sched_yield();
    }
    
    //we were woken - thread_wake_one added us to run queue with READY state
    //but we're continuing directly (not via scheduler) so clean up
    //remove from run queue and mark as RUNNING
    sched_remove(current);
    current->state = THREAD_STATE_RUNNING;
}

void thread_wake_one(wait_queue_t *wq) {
    irq_state_t flags = arch_irq_save();
    
    thread_t *thread = wq->head;
    if (thread) {
        //remove from wait queue
        wq->head = thread->wait_next;
        if (!wq->head) {
            wq->tail = NULL;
        }
        thread->wait_next = NULL;
        
        //mark as ready and add back to run queue
        thread->state = THREAD_STATE_READY;
        sched_add(thread);
    }
    
    arch_irq_restore(flags);
}

void thread_wake_all(wait_queue_t *wq) {
    irq_state_t flags = arch_irq_save();
    
    while (wq->head) {
        thread_t *thread = wq->head;
        wq->head = thread->wait_next;
        thread->wait_next = NULL;
        thread->state = THREAD_STATE_READY;
        sched_add(thread);
    }
    wq->tail = NULL;
    
    arch_irq_restore(flags);
}

//sleep while atomically releasing a held spinlock to prevent missed wakeups
void thread_sleep_locked(wait_queue_t *wq, spinlock_t *lock) {
    thread_t *current = thread_current();
    if (!current) return;
    
    irq_state_t flags = arch_irq_save();
    
    current->state = THREAD_STATE_BLOCKED;
    current->wait_next = NULL;
    if (wq->tail) {
        wq->tail->wait_next = current;
    } else {
        wq->head = current;
    }
    wq->tail = current;
    
    //release the caller's lock BEFORE sleeping so wakers can acquire it
    spinlock_release(lock);
    arch_irq_restore(flags);
    
    while (current->state == THREAD_STATE_BLOCKED) {
        sched_yield();
    }
    
    //reacquire caller's lock before returning
    spinlock_acquire(lock);
    
    //we were woken - thread_wake_one added us to run queue with READY state
    //but we're continuing directly (not via scheduler) so clean up
    sched_remove(current);
    current->state = THREAD_STATE_RUNNING;
}

void thread_sleep_locked_irq(wait_queue_t *wq, spinlock_irq_t *lock, irq_state_t *flags) {
    thread_t *current = thread_current();
    if (!current || !flags) return;

    //caller holds lock with interrupts disabled via spinlock_irq_acquire()
    current->state = THREAD_STATE_BLOCKED;
    current->wait_next = NULL;
    if (wq->tail) {
        wq->tail->wait_next = current;
    } else {
        wq->head = current;
    }
    wq->tail = current;

    //atomically drop the lock, then restore interrupt state and sleep
    spinlock_release(&lock->lock);
    arch_irq_restore(*flags);

    while (current->state == THREAD_STATE_BLOCKED) {
        sched_yield();
    }

    //reacquire the caller's lock and refresh irq flags for caller's context
    *flags = spinlock_irq_acquire(lock);

    //waker queued us READY, but we're still running; normalize state
    sched_remove(current);
    current->state = THREAD_STATE_RUNNING;
}
