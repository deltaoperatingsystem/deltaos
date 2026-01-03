#ifndef PROC_WAIT_H
#define PROC_WAIT_H

#include <arch/types.h>

struct thread;

//wait queue - threads waiting for some event
typedef struct wait_queue {
    struct thread *head;
    struct thread *tail;
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

#endif
