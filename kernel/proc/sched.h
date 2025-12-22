#ifndef PROC_SCHED_H
#define PROC_SCHED_H

#include <proc/thread.h>

//initialize scheduler
void sched_init(void);

//add thread to run queue
void sched_add(thread_t *thread);

//remove thread from run queue
void sched_remove(thread_t *thread);

//yield current thread (cooperative)
void sched_yield(void);

//called from timer interrupt for preemptive scheduling
void sched_tick(void);

//start the scheduler (never returns)
void sched_start(void);

#endif
