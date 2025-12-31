#include <syscall/syscall.h>
#include <drivers/serial.h>
#include <drivers/vt/vt.h>
#include <proc/process.h>
#include <kernel/elf64.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <obj/handle.h>
#include <arch/cpu.h>
#include <mm/pmm.h>
#include <lib/io.h>

static int64 sys_exit(int64 status);
static int64 sys_getpid(void);
static int64 sys_yield(void);
static int64 sys_debug_write(const char *buf, size count);
static int64 sys_write(const char *buf, size count);
static int64 sys_spawn(const char *path, int argc, char **argv);

int64 syscall_dispatch(uint64 num, uint64 arg1, uint64 arg2, uint64 arg3,
                       uint64 arg4, uint64 arg5, uint64 arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    
    switch (num) {
        case SYS_EXIT:
            return sys_exit((int64)arg1);
        
        case SYS_GETPID:
            return sys_getpid();
        
        case SYS_YIELD:
            return sys_yield();
        
        case SYS_DEBUG_WRITE:
            return sys_debug_write((const char *)arg1, (size)arg2);
        
        case SYS_WRITE:
            return sys_write((const char *)arg1, (size)arg2);

        case SYS_SPAWN:
            return sys_spawn((const char *)arg1, (int)arg2, (char **)arg3);
        
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

static int64 sys_spawn(const char *path, int argc, char **argv) {
    // open the file
    char buffer[256];
    snprintf(buffer, 255, "$files/%s", path);
    handle_t h = handle_open(buffer, HANDLE_RIGHT_READ);
    if (h == INVALID_HANDLE) return -1;

    // read the binary
    char buf[8192];
    ssize len = handle_read(h, buf, sizeof(buf));
    handle_close(h);

    if (len <= 0) return -2;

    // validate elf
    if (!elf_validate(buf, len)) return -3;

    // create user process
    process_t *proc = process_create_user(path);
    if (!proc) return -4;

    // load elf into user space
    elf_load_info_t info;
    int err = elf_load_user(buf, len, proc->pagemap, &info);
    if (err != ELF_OK) {
        process_destroy(proc);
        return -5;
    }

    // allocate user stack
    uintptr user_stack_base = 0x7FFFFFFFE000ULL;
    size stack_size = 0x2000;

    uintptr stack_phys = (uintptr)pmm_alloc(stack_size / 4096);
    if (!stack_phys) return -6;
    mmu_map_range(proc->pagemap, user_stack_base - stack_size, stack_phys,
                    stack_size / 4096, MMU_FLAG_WRITE | MMU_FLAG_USER);
    
    // setup argc/argv
    uintptr user_stack_top = process_setup_user_stack(stack_phys, user_stack_base,
                                                    stack_size, argc, argv);
    // create user thread
    thread_t *thread = thread_create_user(proc, (void*)info.entry, (void*)user_stack_top);
    if (!thread) return -7;

    // setup kernel stack for syscalls
    percpu_set_kernel_stack((char*)thread->kernel_stack + thread->kernel_stack_size);

    // add thread to scheduler
    sched_add(thread);
}