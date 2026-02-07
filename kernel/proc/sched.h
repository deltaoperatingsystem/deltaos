#ifndef PROC_SCHED_H
#define PROC_SCHED_H

#include <proc/thread.h>

//initialize scheduler
void sched_init(void);

//initialize scheduler for an AP
void sched_init_ap(void);

//add thread to run queue
void sched_add(thread_t *thread);

//remove thread from run queue
void sched_remove(thread_t *thread);

//yield current thread (cooperative)
void sched_yield(void);

//called from timer interrupt for preemptive scheduling
void sched_tick(int from_usermode);

//start the scheduler (never returns - idle thread runs when no work)
void sched_start(void);

//exit current thread and schedule next (never returns)
void sched_exit(void);

//reap dead threads
void sched_reap(void);

#endif
