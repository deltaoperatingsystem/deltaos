#include <system.h>
#include <sys/syscall.h>

int proc_send_event(uintptr pid, uint32 event) {
    return (int)__syscall2(SYS_PROC_SEND_EVENT, (long)pid, (long)event);
}

int proc_set_event_handler(uint32 event, proc_event_handler_t handler, uint32 flags) {
    return (int)__syscall3(SYS_PROC_SET_EVENT_HANDLER, (long)event, (long)handler, (long)flags);
}

int proc_mask_events(proc_event_mask_t mask) {
    return (int)__syscall1(SYS_PROC_MASK_EVENTS, (long)mask);
}

int proc_unmask_events(proc_event_mask_t mask) {
    return (int)__syscall1(SYS_PROC_UNMASK_EVENTS, (long)mask);
}

int proc_get_pending_events(proc_event_mask_t *out_mask) {
    return (int)__syscall1(SYS_PROC_GET_PENDING_EVENTS, (long)out_mask);
}

int proc_event_return(void) {
    return (int)__syscall0(SYS_PROC_EVENT_RETURN);
}

int proc_set_console_foreground(uintptr pid) {
    return (int)__syscall1(SYS_PROC_SET_CONSOLE_FOREGROUND, (long)pid);
}
