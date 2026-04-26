#ifndef PROC_EVENT_H
#define PROC_EVENT_H

#include <arch/types.h>

struct process;
struct thread;

//bitset of pending or blocked process events
typedef uint64 proc_event_mask_t;

typedef enum {
    PROC_EVENT_TERMINATE = 0, //unconditional process termination request
    PROC_EVENT_INTERRUPT = 1, //foreground interrupt like ctrl+C
    PROC_EVENT_CHILD     = 2, //one of this process' children changed state
    PROC_EVENT_WAKE      = 3, //reserved for breaking blocking waits
    PROC_EVENT_USER0     = 4, //application-defined event
    PROC_EVENT_USER1     = 5, //application-defined event
    PROC_EVENT_COUNT
} proc_event_t;

#define PROC_EVENT_BIT(event) (1ULL << (event))
#define PROC_EVENT_MASK_ALL ((PROC_EVENT_BIT(PROC_EVENT_COUNT) - 1ULL))

typedef struct proc_event_action {
    uint64 entry;    //userspace handler RIP, or 0 for default action
    uint32 flags;    //reserved for future delivery options
    uint32 reserved;
} proc_event_action_t;

//queue an event for delivery at the target process' next safe user boundary
int proc_post_event(struct process *proc, uint32 event);

//install or clear a userspace handler for a catchable event
int proc_set_event_handler(struct process *proc, uint32 event, uint64 entry, uint32 flags);

//snapshot currently pending events without consuming them
int proc_get_pending_events(struct process *proc, proc_event_mask_t *out_mask);

//deliver pending events against thread->context before interrupt return
int proc_deliver_pending(struct thread *thread);

//prepare thread->user_context before returning from a syscall
void proc_prepare_syscall_return(struct thread *thread, intptr retval);

//used by long blocking kernel loops to notice Ctrl+C/terminate promptly
int proc_current_should_abort_blocking(void);

//console foreground owner receives keyboard-generated interrupt events
int proc_set_console_foreground(struct process *caller, uint64 pid);
uint64 proc_get_console_foreground_pid(void);
void proc_clear_console_foreground_if_owner(uint64 pid);

#endif
