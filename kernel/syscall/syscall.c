#include <syscall/syscall.h>
#include <drivers/serial.h>
#include <drivers/vt/vt.h>
#include <proc/process.h>
#include <proc/thread.h>
#include <proc/sched.h>

static int64 sys_exit(int64 status);
static int64 sys_getpid(void);
static int64 sys_yield(void);
static int64 sys_debug_write(const char *buf, size count);
static int64 sys_write(const char *buf, size count);

int64 syscall_dispatch(uint64 num, uint64 arg1, uint64 arg2, uint64 arg3,
                       uint64 arg4, uint64 arg5, uint64 arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    
    switch (num) {
        case SYS_exit:
            return sys_exit((int64)arg1);
        
        case SYS_getpid:
            return sys_getpid();
        
        case SYS_yield:
            return sys_yield();
        
        case SYS_debug_write:
            return sys_debug_write((const char *)arg1, (size)arg2);
        
        case SYS_write:
            return sys_write((const char *)arg1, (size)arg2);
        
        default:
            return -1;
    }
}

static int64 sys_exit(int64 status) {
    (void)status;
    thread_exit();
    return 0;
}

static int64 sys_getpid(void) {
    process_t *proc = process_current();
    if (proc) {
        return (int64)proc->pid;
    }
    return 0;
}

static int64 sys_yield(void) {
    sched_yield();
    return 0;
}

static int64 sys_debug_write(const char *buf, size count) {
    if (!buf) return -1;
    for (size i = 0; i < count; i++) {
        serial_write_char(buf[i]);
    }
    return (int64)count;
}

static int64 sys_write(const char *buf, size count) {
    if (!buf) return -1;
    
    vt_t *vt = vt_get_active();
    if (!vt) return -1;
    
    vt_write(vt, buf, count);
    vt_flush(vt);
    
    return (int64)count;
}
