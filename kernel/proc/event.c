#include <proc/event.h>
#include <proc/process.h>
#include <proc/thread.h>

static spinlock_irq_t console_fg_lock = SPINLOCK_IRQ_INIT;
static uint64 console_foreground_pid = 0;

static int proc_event_is_deliverable_to_handler(uint32 event) {
    return event != PROC_EVENT_TERMINATE && event != PROC_EVENT_WAKE;
}

static int proc_event_default_consumes(uint32 event) {
    return event == PROC_EVENT_TERMINATE || event == PROC_EVENT_INTERRUPT;
}

static int proc_event_default_action(thread_t *thread, uint32 event) {
    process_t *proc;

    if (!thread || !thread->process) return 0;
    proc = thread->process;

    if (event == PROC_EVENT_TERMINATE) {
        process_exit(proc, -1);
        if (thread == thread_current()) thread_exit();
        return 1;
    }

    if (event == PROC_EVENT_INTERRUPT) {
        process_exit(proc, 130);
        if (thread == thread_current()) thread_exit();
        return 1;
    }

    return 0;
}

int proc_post_event(process_t *proc, uint32 event) {
    if (!proc || event >= PROC_EVENT_COUNT) return -1;

    irq_state_t flags = spinlock_irq_acquire(&proc->event_lock);
    proc->pending_events |= PROC_EVENT_BIT(event);
    spinlock_irq_release(&proc->event_lock, flags);
    return 0;
}

int proc_set_event_handler(process_t *proc, uint32 event, uint64 entry, uint32 flags) {
    if (!proc || event >= PROC_EVENT_COUNT) return -1;
    if (!proc_event_is_deliverable_to_handler(event) && entry != 0) return -1;

    irq_state_t irq_flags = spinlock_irq_acquire(&proc->event_lock);
    proc->event_actions[event].entry = entry;
    proc->event_actions[event].flags = flags;
    spinlock_irq_release(&proc->event_lock, irq_flags);
    return 0;
}

int proc_get_pending_events(process_t *proc, proc_event_mask_t *out_mask) {
    if (!proc || !out_mask) return -1;

    irq_state_t flags = spinlock_irq_acquire(&proc->event_lock);
    *out_mask = proc->pending_events;
    spinlock_irq_release(&proc->event_lock, flags);
    return 0;
}

static int proc_deliver_pending_to_ctx(thread_t *thread, arch_context_t *ctx, uint8 restore_slot) {
    process_t *proc;
    proc_event_mask_t deliverable;
    proc_event_action_t action = {0};
    uint32 event = 0;
    int found = 0;

    if (!thread || !thread->process || !ctx) return 0;
    if ((ctx->cs & 3) != 3) return 0;
    irq_state_t thread_flags = spinlock_irq_acquire(&thread->lock);
    if (thread->in_event_handler) {
        spinlock_irq_release(&thread->lock, thread_flags);
        return 0;
    }
    proc_event_mask_t blocked = thread->blocked_events;
    spinlock_irq_release(&thread->lock, thread_flags);

    proc = thread->process;

    irq_state_t event_flags = spinlock_irq_acquire(&proc->event_lock);
    deliverable = proc->pending_events & ~blocked & PROC_EVENT_MASK_ALL;
    for (event = 0; event < PROC_EVENT_COUNT; event++) {
        if (deliverable & PROC_EVENT_BIT(event)) {
            action = proc->event_actions[event];
            found = 1;
            break;
        }
    }

    if (found &&
        ((action.entry != 0 && proc_event_is_deliverable_to_handler(event)) ||
         proc_event_default_consumes(event))) {
        proc->pending_events &= ~PROC_EVENT_BIT(event);
    }
    spinlock_irq_release(&proc->event_lock, event_flags);

    if (!found) return 0;

    //uncaught events fall back to kernel policy e.x INTERRUPT terminates
    //the foreground process without needing a userspace handler installed
    if (action.entry == 0 || !proc_event_is_deliverable_to_handler(event)) {
        return proc_event_default_action(thread, event);
    }

    //save the interrupted userspace image and redirect the chosen return
    //context to the handler then handler resumes via proc_event_return
    thread_flags = spinlock_irq_acquire(&thread->lock);
    thread->saved_event_context = *ctx;
    thread->saved_blocked_events = thread->blocked_events;
    thread->blocked_events |= PROC_EVENT_BIT(event);
    thread->in_event_handler = 1;
    thread->event_returning = 0;
    thread->event_restore_slot = restore_slot;

    ctx->rip = action.entry;
    ctx->rdi = event;
    ctx->rax = 0;
    spinlock_irq_release(&thread->lock, thread_flags);
    return 1;
}

int proc_deliver_pending(thread_t *thread) {
    return proc_deliver_pending_to_ctx(thread, &thread->context, EVENT_RESTORE_KERNEL_CTX);
}

void proc_prepare_syscall_return(thread_t *thread, intptr retval) {
    if (!thread) return;

    //if this syscall is proc_event_return() it already restored the saved
    //userspace context do not overwrite its original RAX with this syscalls 0
    if (thread->event_returning) {
        irq_state_t flags = spinlock_irq_acquire(&thread->lock);
        thread->event_returning = 0;
        spinlock_irq_release(&thread->lock, flags);
    } else {
        thread->user_context.rax = retval;
    }

    proc_deliver_pending_to_ctx(thread, &thread->user_context, EVENT_RESTORE_USER_CONTEXT);
}

int proc_current_should_abort_blocking(void) {
    thread_t *thread = thread_current();
    process_t *proc;
    proc_event_mask_t abort_mask;
    int should_abort;

    if (!thread || !thread->process) return 0;
    proc = thread->process;

    //blocking syscalls poll this so console interrupts are not delayed until
    //a DNS/TCP timeout or some other long wait naturally finishes
    abort_mask = PROC_EVENT_BIT(PROC_EVENT_TERMINATE) | PROC_EVENT_BIT(PROC_EVENT_INTERRUPT);

    irq_state_t thread_flags = spinlock_irq_acquire(&thread->lock);
    proc_event_mask_t blocked = thread->blocked_events;
    spinlock_irq_release(&thread->lock, thread_flags);

    irq_state_t event_flags = spinlock_irq_acquire(&proc->event_lock);
    should_abort = (proc->pending_events & ~blocked & abort_mask) != 0;
    spinlock_irq_release(&proc->event_lock, event_flags);
    return should_abort;
}

int proc_set_console_foreground(process_t *caller, uint64 pid) {
    process_t *target;

    if (!caller) return -1;
    if (pid == 0) {
        irq_state_t flags = spinlock_irq_acquire(&console_fg_lock);
        if (console_foreground_pid != 0 && console_foreground_pid != caller->pid) {
            spinlock_irq_release(&console_fg_lock, flags);
            return -1;
        }
        console_foreground_pid = 0;
        spinlock_irq_release(&console_fg_lock, flags);
        return 0;
    }

    target = process_find_ref(pid);
    if (!target) return -1;

    //for now the shell may hand foreground control to itself or its child processes only
    //this keeps random processes from stealing ctrl+C focus
    if (pid != caller->pid && target->parent_pid != caller->pid) {
        process_unref(target);
        return -1;
    }

    irq_state_t flags = spinlock_irq_acquire(&console_fg_lock);
    console_foreground_pid = pid;
    spinlock_irq_release(&console_fg_lock, flags);
    process_unref(target);
    return 0;
}

uint64 proc_get_console_foreground_pid(void) {
    uint64 pid;

    irq_state_t flags = spinlock_irq_acquire(&console_fg_lock);
    pid = console_foreground_pid;
    spinlock_irq_release(&console_fg_lock, flags);
    return pid;
}

void proc_clear_console_foreground_if_owner(uint64 pid) {
    irq_state_t flags = spinlock_irq_acquire(&console_fg_lock);
    if (console_foreground_pid == pid) {
        console_foreground_pid = 0;
    }
    spinlock_irq_release(&console_fg_lock, flags);
}
