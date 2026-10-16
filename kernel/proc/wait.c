#include <proc/wait.h>
#include <proc/process.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <arch/interrupts.h>
#include <arch/cpu.h>
#include <arch/percpu.h>
#include <lib/io.h>
#include <lib/spinlock.h>

void wait_queue_init(wait_queue_t *wq) {
    wq->head = NULL;
    wq->tail = NULL;
    spinlock_irq_init(&wq->lock);
}

void thread_sleep(wait_queue_t *wq) {
    thread_t *current = thread_current();
    if (!current) return;

    irq_state_t flags = spinlock_irq_acquire(&wq->lock);

    //mark as blocked
    current->state = THREAD_STATE_BLOCKED;
    current->wait_cpu = (int)percpu_get()->cpu_index;

    //add to wait queue
    current->wait_next = NULL;
    current->blocked_on = wq;
    if (wq->tail) {
        wq->tail->wait_next = current;
    } else {
        wq->head = current;
    }
    wq->tail = current;

    spinlock_irq_release(&wq->lock, flags);

    //wait until woken
    while (__atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == THREAD_STATE_BLOCKED) {
        sched_yield();
    }

    //wakers hold wq->lock across state=READY + sched_add_cpu, so acquiring
    //wq->lock here ensures the enqueue completed before we dequeue ourselves
    irq_state_t wflags = spinlock_irq_acquire(&wq->lock);
    //we were woken - thread_wake_one added us to run queue with READY state
    //but we're continuing directly (not via scheduler) so clean up
    //remove from run queue and mark as RUNNING
    sched_remove(current);
    current->state = THREAD_STATE_RUNNING;
    current->blocked_on = NULL;
    spinlock_irq_release(&wq->lock, wflags);
}

void thread_wake_one(wait_queue_t *wq) {
    irq_state_t flags = spinlock_irq_acquire(&wq->lock);

    thread_t *thread = wq->head;
    if (thread) {
        irq_state_t t_flags = spinlock_irq_acquire(&thread->lock);

        //remove from wait queue
        wq->head = thread->wait_next;
        if (!wq->head) {
            wq->tail = NULL;
        }
        thread->wait_next = NULL;
        thread->blocked_on = NULL;

        //mark as ready and add back to run queue if not currently running
        if (thread->process && thread->process->state == PROC_STATE_DEAD) {
            thread->wait_cpu = -1;
            spinlock_irq_release(&thread->lock, t_flags);
            sched_queue_dead(thread);
        } else {
            thread->state = THREAD_STATE_READY;
            uint32 target_cpu;
            if (thread->cpu_id < 0) {
                target_cpu = (thread->wait_cpu >= 0) ? (uint32)thread->wait_cpu
                                                     : percpu_get()->cpu_index;
            } else {
                target_cpu = (uint32)thread->cpu_id;
            }
            thread->wait_cpu = -1;
            spinlock_irq_release(&thread->lock, t_flags);
            sched_add_cpu(thread, target_cpu);
        }
    }

    spinlock_irq_release(&wq->lock, flags);
}

void thread_wake_all(wait_queue_t *wq) {
    irq_state_t flags = spinlock_irq_acquire(&wq->lock);

    while (wq->head) {
        thread_t *thread = wq->head;
        irq_state_t t_flags = spinlock_irq_acquire(&thread->lock);

        wq->head = thread->wait_next;
        thread->wait_next = NULL;
        thread->blocked_on = NULL;
        if (thread->process && thread->process->state == PROC_STATE_DEAD) {
            thread->wait_cpu = -1;
            spinlock_irq_release(&thread->lock, t_flags);
            sched_queue_dead(thread);
        } else {
            thread->state = THREAD_STATE_READY;
            uint32 target_cpu;
            if (thread->cpu_id < 0) {
                target_cpu = (thread->wait_cpu >= 0) ? (uint32)thread->wait_cpu
                                                     : percpu_get()->cpu_index;
            } else {
                target_cpu = (uint32)thread->cpu_id;
            }
            thread->wait_cpu = -1;
            spinlock_irq_release(&thread->lock, t_flags);
            sched_add_cpu(thread, target_cpu);
        }
    }
    wq->tail = NULL;

    spinlock_irq_release(&wq->lock, flags);
}

//sleep while atomically releasing a held spinlock to prevent missed wakeups
void thread_sleep_locked(wait_queue_t *wq, spinlock_t *lock) {
    thread_t *current = thread_current();
    if (!current) return;

    irq_state_t flags = spinlock_irq_acquire(&wq->lock);

    current->state = THREAD_STATE_BLOCKED;
    current->wait_cpu = (int)percpu_get()->cpu_index;
    current->blocked_on = wq;
    current->wait_next = NULL;
    if (wq->tail) {
        wq->tail->wait_next = current;
    } else {
        wq->head = current;
    }
    wq->tail = current;

    //release wait-queue lock then caller lock before sleeping so wakers can acquire both
    spinlock_irq_release(&wq->lock, flags);
    spinlock_release(lock);

    while (__atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == THREAD_STATE_BLOCKED) {
        sched_yield();
    }

    //reacquire caller's lock before returning
    spinlock_acquire(lock);

    //wait for the waker's enqueue to finish before dequeuing
    irq_state_t wflags = spinlock_irq_acquire(&wq->lock);
    //we were woken - thread_wake_one added us to run queue with READY state
    //but we're continuing directly (not via scheduler) so clean up
    sched_remove(current);
    current->state = THREAD_STATE_RUNNING;
    current->blocked_on = NULL;
    spinlock_irq_release(&wq->lock, wflags);
}

void thread_sleep_locked_irq(wait_queue_t *wq, spinlock_irq_t *lock, irq_state_t *flags) {
    thread_t *current = thread_current();
    if (!current || !flags) return;

    //caller holds lock with interrupts disabled via spinlock_irq_acquire()
    irq_state_t wq_flags = spinlock_irq_acquire(&wq->lock);
    current->state = THREAD_STATE_BLOCKED;
    current->wait_cpu = (int)percpu_get()->cpu_index;
    current->blocked_on = wq;
    current->wait_next = NULL;
    if (wq->tail) {
        wq->tail->wait_next = current;
    } else {
        wq->head = current;
    }
    wq->tail = current;
    spinlock_irq_release(&wq->lock, wq_flags);

    //atomically drop the lock, then restore interrupt state and sleep
    spinlock_release(&lock->lock);
    arch_irq_restore(*flags);

    while (__atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == THREAD_STATE_BLOCKED) {
        sched_yield();
    }

    //reacquire the caller's lock and refresh irq flags for caller's context
    *flags = spinlock_irq_acquire(lock);

    //wait for the waker's enqueue to finish before dequeuing
    irq_state_t wflags = spinlock_irq_acquire(&wq->lock);
    //waker queued us READY, but we're still running; normalize state
    sched_remove(current);
    current->state = THREAD_STATE_RUNNING;
    current->blocked_on = NULL;
    spinlock_irq_release(&wq->lock, wflags);
}

void thread_wake_thread(thread_t *thread) {
    if (!thread) return;

    //sample wq under thread->lock, then acquire in wq->lock -> thread->lock order
    for (int iter = 0; iter < 4; iter++) {
        irq_state_t tflags = spinlock_irq_acquire(&thread->lock);
        wait_queue_t *wq = thread->blocked_on;
        int blocked = (thread->state == THREAD_STATE_BLOCKED);
        spinlock_irq_release(&thread->lock, tflags);
        if (!wq || !blocked) return;

        irq_state_t wflags = spinlock_irq_acquire(&wq->lock);
        tflags = spinlock_irq_acquire(&thread->lock);
        if (thread->blocked_on != wq || thread->state != THREAD_STATE_BLOCKED) {
            spinlock_irq_release(&thread->lock, tflags);
            spinlock_irq_release(&wq->lock, wflags);
            return;
        }
        //unlink from this wq
        {
            thread_t *prev = NULL;
            thread_t *curr = wq->head;
            int removed = 0;
            while (curr) {
                if (curr == thread) {
                    if (prev) prev->wait_next = curr->wait_next;
                    else wq->head = curr->wait_next;
                    if (wq->tail == curr) wq->tail = prev;
                    curr->wait_next = NULL;
                    curr->blocked_on = NULL;
                    removed = 1;
                    break;
                }
                prev = curr;
                curr = curr->wait_next;
            }
            if (!removed) {
                spinlock_irq_release(&thread->lock, tflags);
                spinlock_irq_release(&wq->lock, wflags);
                return;
            }
        }
        if (thread->process && thread->process->state == PROC_STATE_DEAD) {
            thread->wait_cpu = -1;
            spinlock_irq_release(&thread->lock, tflags);
            spinlock_irq_release(&wq->lock, wflags);
            sched_queue_dead(thread);
            return;
        }
        thread->state = THREAD_STATE_READY;
        uint32 target_cpu;
        if (thread->cpu_id < 0) {
            target_cpu = (thread->wait_cpu >= 0) ? (uint32)thread->wait_cpu
                                                 : percpu_get()->cpu_index;
        } else {
            target_cpu = (uint32)thread->cpu_id;
        }
        thread->wait_cpu = -1;
        spinlock_irq_release(&thread->lock, tflags);
        //hold wq->lock across sched_add_cpu so sleepers see the completed enqueue
        sched_add_cpu(thread, target_cpu);
        spinlock_irq_release(&wq->lock, wflags);
        return;
    }
}
