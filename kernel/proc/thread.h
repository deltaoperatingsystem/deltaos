#ifndef PROC_THREAD_H
#define PROC_THREAD_H

#include <arch/types.h>
#include <arch/context.h>
#include <lib/spinlock.h>
#include <obj/object.h>

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
    
    //kernel object wrapper (for capability-based access)
    object_t *obj;
    
    //entry point and argument (for kernel trampoline)
    void (*entry)(void *);
    void *arg;
    
    //arch-opaque saved context
    arch_context_t context;
    
    //kernel stack for this thread
    void *kernel_stack;
    size kernel_stack_size;

    //usermode state for initial entry (only for user threads)
    arch_context_t user_context;
    
    //scheduler state
    int cpu_id;             //ID of CPU currently running this thread (-1 if none)
    spinlock_t lock;        //thread-specific lock for state changes

    //linked list within process
    struct thread *next;
    
    //scheduler queue link
    struct thread *sched_next;
    
    //wait queue link (for blocking)
    struct thread *wait_next;
} thread_t;

//create a thread in a process
thread_t *thread_create(struct process *proc, void (*entry)(void *), void *arg);

//destroy a thread (frees resources)
void thread_destroy(thread_t *thread);

//get the thread as a kernel object (for granting handles to threads)
object_t *thread_get_object(thread_t *thread);

//create a usermode thread (entry/stack are in user address space)
thread_t *thread_create_user(struct process *proc, void *entry, void *user_stack);

//exit current thread (never returns)
void thread_exit(void);

//get current thread
thread_t *thread_current(void);

//set current thread
void thread_set_current(thread_t *thread);

#endif

