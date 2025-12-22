#ifndef PROC_THREAD_H
#define PROC_THREAD_H

#include <arch/types.h>
#include <arch/context.h>

struct process;

//thread states
#define THREAD_STATE_READY   0
#define THREAD_STATE_RUNNING 1
#define THREAD_STATE_BLOCKED 2
#define THREAD_STATE_DEAD    3

//thread structure
typedef struct thread {
    uint64 tid;
    struct process *process;
    uint32 state;
    
    //arch-opaque saved context
    arch_context_t context;
    
    //kernel stack for this thread
    void *kernel_stack;
    size kernel_stack_size;
    
    //linked list within process
    struct thread *next;
    
    //scheduler queue link
    struct thread *sched_next;
} thread_t;

//create a thread in a process
thread_t *thread_create(struct process *proc, void (*entry)(void *), void *arg);

//destroy a thread
void thread_destroy(thread_t *thread);

//get current thread
thread_t *thread_current(void);

//set current thread
void thread_set_current(thread_t *thread);

#endif
