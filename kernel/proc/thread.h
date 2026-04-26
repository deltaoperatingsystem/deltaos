#ifndef PROC_THREAD_H
#define PROC_THREAD_H

#include <arch/types.h>
#include <arch/context.h>
#include <arch/fpu.h>
#include <proc/event.h>
#include <lib/spinlock.h>
#include <obj/object.h>

struct process;

//thread states
#define THREAD_STATE_READY   0
#define THREAD_STATE_RUNNING 1
#define THREAD_STATE_BLOCKED 2
#define THREAD_STATE_DEAD    3

typedef enum event_restore_slot {
    EVENT_RESTORE_KERNEL_CTX = 0,
    EVENT_RESTORE_USER_CONTEXT = 1
} event_restore_slot_t;

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

    //saved userspace state while an async event handler is running
    arch_context_t saved_event_context;

    //saved x87/SSE state for this thread
    arch_fpu_state_t fpu_state;
    uint8 fpu_used;

    //events masked on this thread are left pending on the process
    proc_event_mask_t blocked_events;

    //previous mask restored by proc_event_return()
    proc_event_mask_t saved_blocked_events;

    //prevents nested handler delivery in the first async-event implementation
    uint8 in_event_handler;

    //set by proc_event_return() so syscall return preserves restored RAX
    uint8 event_returning;

    //which context slot should proc_event_return() restore into
    uint8 event_restore_slot;
    
    //scheduler state
    int cpu_id;             //ID of CPU currently running this thread (-1 if none)
    int wait_cpu;           //CPU this thread blocked on (for targeted wakeups)
    spinlock_irq_t lock;    //thread-specific lock for state changes

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
