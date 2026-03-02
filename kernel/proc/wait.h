#ifndef PROC_WAIT_H
#define PROC_WAIT_H

#include <arch/types.h>
#include <lib/spinlock.h>

struct thread;

//wait queue - threads waiting for some event
typedef struct wait_queue {
    struct thread *head;
    struct thread *tail;
    spinlock_irq_t lock;
} wait_queue_t;

//initialize a wait queue
void wait_queue_init(wait_queue_t *wq);

//put current thread to sleep on wait queue
//removes from run queue, adds to wait queueand reschedules
void thread_sleep(wait_queue_t *wq);

//wake one thread from wait queue
//removes from wait queue, adds to run queue
void thread_wake_one(wait_queue_t *wq);

//wake all threads from wait queue
void thread_wake_all(wait_queue_t *wq);

//sleep while holding a spinlock: atomically releases lock, sleeps, reacquires on wake
void thread_sleep_locked(wait_queue_t *wq, spinlock_t *lock);

//sleep while atomically releasing a held spinlock_irq_t to prevent missed wakeups
//reacquires the lock and updates *flags before returning
void thread_sleep_locked_irq(wait_queue_t *wq, spinlock_irq_t *lock, irq_state_t *flags);

#endif
