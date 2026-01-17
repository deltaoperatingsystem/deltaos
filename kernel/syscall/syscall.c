#include <syscall/syscall.h>
#include <drivers/serial.h>
#include <proc/process.h>
#include <kernel/elf64.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <obj/handle.h>
#include <obj/namespace.h>
#include <ipc/channel.h>
#include <mm/vmo.h>
#include <arch/cpu.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <lib/io.h>
#include <lib/string.h>

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
    debug_write(buf, count);
    return (int64)count;
}

static int64 sys_spawn(const char *path, int argc, char **argv) {
    //open path
    handle_t h = handle_open(path, HANDLE_RIGHT_READ);
    if (h == INVALID_HANDLE) {
        return -1;
    }
    
    //allocate buffer for exec binary
    size buf_size = 32768;  //32KB should be enough
    char *buf = kzalloc(buf_size);
    if (!buf) {
        return -1;
    }
    
    ssize len = handle_read(h, buf, buf_size);
    handle_close(h);
    
    if (len <= 0) {
        kfree(buf);
        return -1;
    }
    
    //validate ELF
    if (!elf_validate(buf, len)) {
        kfree(buf);
        return -1;
    }
    
    //create suspended user process (capability-based model)
    //the process starts empty - we load the ELF and configure it before starting
    process_t *proc = process_create_user_suspended(path);
    if (!proc) {
        kfree(buf);
        return -1;
    }
    
    //load ELF into user address space
    elf_load_info_t info;
    int err = elf_load_user(buf, len, proc, &info);
    if (err != ELF_OK) {
        process_destroy(proc);
        kfree(buf);
        return -1;
    }
    
    //check for dynamic executable (has interpreter)
    uint64 interp_base = 0;
    uint64 real_entry = info.entry;
    
    if (info.interp_path[0]) {
        
        //convert interpreter path to initrd path
        //e.x /system/libraries/ld.so -> $files/initrd/system/libraries/ld.so
        char interp_fullpath[256];
        if (info.interp_path[0] == '/') {
            //absolute path - prepend $files/initrd
            snprintf(interp_fullpath, sizeof(interp_fullpath), "$files/initrd%s", info.interp_path);
        } else {
            snprintf(interp_fullpath, sizeof(interp_fullpath), "$files/initrd/%s", info.interp_path);
        }
        
        //load interpreter
        handle_t ih = handle_open(interp_fullpath, HANDLE_RIGHT_READ);
        if (ih == INVALID_HANDLE) {
            process_destroy(proc);
            kfree(buf);
            return -1;
        }
        
        size interp_buf_size = 32768; //32KB for interpreter
        char *interp_buf = kzalloc(interp_buf_size);
        if (!interp_buf) {
            handle_close(ih);
            process_destroy(proc);
            kfree(buf);
            return -1;
        }

        ssize interp_len = handle_read(ih, interp_buf, interp_buf_size);
        handle_close(ih);
        
        if (interp_len <= 0 || !elf_validate(interp_buf, interp_len)) {
            process_destroy(proc);
            kfree(interp_buf);
            kfree(buf);
            return -1;
        }
        
        //load interpreter into address space
        elf_load_info_t interp_info;
        err = elf_load_user(interp_buf, interp_len, proc, &interp_info);
        if (err != ELF_OK) {
            process_destroy(proc);
            kfree(interp_buf);
            kfree(buf);
            return -1;
        }
        
        interp_base = interp_info.virt_base;
        real_entry = interp_info.entry;  //jump to interpreter not executable
        kfree(interp_buf);
    }
    
    //allocate user stack
    uintptr user_stack_base = 0x7FFFFFFFE000ULL;
    size stack_size = 0x2000;
    
    uintptr stack_phys = (uintptr)pmm_alloc(stack_size / 4096);
    if (!stack_phys) {
        return -1;
    }
    
    mmu_map_range(proc->pagemap, user_stack_base - stack_size, stack_phys, 
                  stack_size / 4096, MMU_FLAG_WRITE | MMU_FLAG_USER);
    
    //track stack in VMA list for cleanup
    process_vma_add(proc, user_stack_base - stack_size, stack_size, 
                    MMU_FLAG_WRITE | MMU_FLAG_USER, NULL, 0);
    

    //set up argc/argv and aux vector
    
    uintptr user_stack_top;
    if (info.interp_path[0]) {
        //dynamic executable: use aux vector stack setup
        user_stack_top = process_setup_user_stack_dynamic(
            stack_phys, user_stack_base, stack_size, argc, argv,
            info.phdr_addr, info.phdr_count, info.phdr_size,
            info.entry,  //AT_ENTRY = original program entry
            interp_base  //AT_BASE = interpreter load address
        );
        
    } else {
        //static executable: simple stack setup
        user_stack_top = process_setup_user_stack(stack_phys, user_stack_base, 
                                                   stack_size, argc, argv);
    }
    
    
    //create user thread
    thread_t *thread = thread_create_user(proc, (void*)real_entry, (void*)user_stack_top);
    if (!thread) {
        kfree(buf);
        return -1;
    }
    
    //add exec thread to scheduler
    sched_add(thread);
    
    kfree(buf);
    return (int64)proc->pid;
}

static int64 sys_wait(uint64 pid) {
    process_t *proc = process_find(pid);
    if (!proc) return -1;
    
    //sleep until the process is destroyed (wakes exit_wait)
    thread_sleep(&proc->exit_wait);
    
    return 0;
}


//get an object handle from a namespace 
//parent: handle to parent namespace/directory or INVALID_HANDLE for root
//path: path within that namespace
//rights: requested rights for the returned handle
static int64 sys_get_obj(handle_t parent, const char *path, handle_rights_t rights) {
    if (!path) return -1;
    
    //if parent is INVALID_HANDLE do a root namespace lookup
    if (parent == INVALID_HANDLE) {
        handle_t h = handle_open(path, rights);
        return h;
    }
    
    //otherwise, parent should be a directory - lookup within it
    object_t *parent_obj = handle_get(parent);
    if (!parent_obj) return -2;
    
    //check parent has read rights (needed to traverse)
    if (!handle_has_rights(parent, HANDLE_RIGHT_READ)) {
        return -3;
    }
    
    //check parent supports lookup
    if (!parent_obj->ops || !parent_obj->ops->lookup) {
        return -4;  //not a directory or doesn't support lookup
    }
    
    //do the lookup
    object_t *child = parent_obj->ops->lookup(parent_obj, path);
    if (!child) {
        return -5;  //not found
    }
    
    //grant handle with requested rights
    process_t *proc = process_current();
    if (!proc) {
        object_deref(child);
        return -6;
    }
    
    int h = process_grant_handle(proc, child, rights);
    object_deref(child);  //grant_handle adds its own ref
    return h;
}

//read from a handle
//read from a handle
static int64 sys_handle_read(handle_t h, void *buf, size len) {
    if (!buf || len == 0) return -1;
    return handle_read(h, buf, len);
}

//write to a handle
static int64 sys_handle_write(handle_t h, const void *buf, size len) {
    if (!buf || len == 0) return -1;
    return handle_write(h, buf, len);
}

static int64 sys_handle_seek(handle_t h, size offset, int mode) {
    return handle_seek(h, offset, mode);
}

//close a handle
static int64 sys_handle_close(handle_t h) {
    process_t *proc = process_current();
    if (!proc) return -1;
    return process_close_handle(proc, h);
}

//duplicate a handle with same or reduced rights
static int64 sys_handle_dup(handle_t h, handle_rights_t new_rights) {
    process_t *proc = process_current();
    if (!proc) return -1;
    return process_duplicate_handle(proc, h, new_rights);
}

//create a virtual memory object
static int64 sys_vmo_create(size size, uint32 flags, handle_rights_t rights) {
    if (size == 0) return -1;
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_create(proc, size, flags, rights);
}

//read from a VMO
static int64 sys_vmo_read(handle_t h, void *buf, size len, size offset) {
    if (!buf || len == 0) return -1;
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_read(proc, h, buf, len, offset);
}

//write to a VMO
static int64 sys_vmo_write(handle_t h, const void *buf, size len, size offset) {
    if (!buf || len == 0) return -1;
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_write(proc, h, buf, len, offset);
}

//map a VMO into the process address space
//returns mapped virtual address or 0 on failure
static int64 sys_vmo_map(handle_t h, uintptr vaddr_hint, size offset, size len, uint32 flags) {
    process_t *proc = process_current();
    if (!proc) return 0;
    
    handle_rights_t map_rights = (handle_rights_t)flags;
    
    void *result = vmo_map(proc, h, (void *)vaddr_hint, offset, len, map_rights);
    return (int64)(uintptr)result;
}

//unmap memory from process address space
static int64 sys_vmo_unmap(uintptr vaddr, size len) {
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_unmap(proc, (void *)vaddr, len);
}

//create a channel pair
//returns two endpoint handles in ep0_out and ep1_out
static int64 sys_channel_create(int32 *ep0_out, int32 *ep1_out) {
    if (!ep0_out || !ep1_out) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    return channel_create(proc, HANDLE_RIGHTS_DEFAULT, ep0_out, ep1_out);
}

//send a message through a channel endpoint
//this is a data-only send (no handle transfer from userspace yet)
static int64 sys_channel_send(handle_t ep, const void *data, size len) {
    if (!data && len > 0) return -1;
    if (len > CHANNEL_MAX_MSG_SIZE) return -2;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    //copy data from userspace to kernel buffer
    void *kbuf = NULL;
    if (len > 0) {
        kbuf = kmalloc(len);
        if (!kbuf) return -3;
        memcpy(kbuf, data, len);
    }
    
    channel_msg_t msg;
    msg.data = kbuf;
    msg.data_len = len;
    msg.handles = NULL;
    msg.handle_count = 0;
    
    int result = channel_send(proc, ep, &msg);
    
    //channel_send copies the data, so free our buffer
    if (kbuf) kfree(kbuf);
    
    return result;
}

//receive a message from a channel endpoint
//returns number of bytes received or negative on error
//ignores transferred handles - use sys_channel_recv_msg for those
static int64 sys_channel_recv(handle_t ep, void *buf, size buflen) {
    if (!buf && buflen > 0) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    //copy data to userspace
    size to_copy = msg.data_len < buflen ? msg.data_len : buflen;
    if (to_copy > 0 && msg.data) {
        memcpy(buf, msg.data, to_copy);
    }
    
    //free the message data
    if (msg.data) kfree(msg.data);
    
    //close any transferred handles (this syscall ignores them)
    for (uint32 i = 0; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    if (msg.handles) kfree(msg.handles);
    
    return (int64)msg.data_len;
}

//receive a message with handles from a channel endpoint
//data_buf: buffer for message data
//data_len: max bytes to copy
//handles_buf: buffer for received handles (array of int32)
//handles_len: max handles
//result_out: receives actual counts
static int64 sys_channel_recv_msg(handle_t ep, void *data_buf, size data_len,
                                  int32 *handles_buf, uint32 handles_len,
                                  channel_recv_result_t *result_out) {
    process_t *proc = process_current();
    if (!proc) return -1;
    if (!result_out) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    //copy data to userspace
    size to_copy = msg.data_len < data_len ? msg.data_len : data_len;
    if (to_copy > 0 && msg.data && data_buf) {
        memcpy(data_buf, msg.data, to_copy);
    }
    
    //copy handles to userspace
    uint32 handles_to_copy = msg.handle_count < handles_len ? msg.handle_count : handles_len;
    if (handles_to_copy > 0 && msg.handles && handles_buf) {
        memcpy(handles_buf, msg.handles, handles_to_copy * sizeof(int32));
    }
    
    //close excess handles that didn't fit in the buffer
    for (uint32 i = handles_to_copy; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    
    //fill result
    result_out->data_len = msg.data_len;
    result_out->handle_count = msg.handle_count;
    result_out->sender_pid = msg.sender_pid;
    
    //cleanup
    if (msg.data) kfree(msg.data);
    if (msg.handles) kfree(msg.handles);
    
    return 0;
}

//register a handle in the namespace
//allows userspace services to publish their objects for other processes to find
static int64 sys_ns_register(const char *path, handle_t h) {
    if (!path) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    //get the object from the handle
    object_t *obj = process_get_handle(proc, h);
    if (!obj) return -2;  //invalid handle
    
    //register in namespace (ns_register adds a reference)
    int result = ns_register(path, obj);
    return result;
}

static int64 sys_stat(const char *path, stat_t *st) {
    return handle_stat(path, st);
}

static int64 sys_channel_try_recv(handle_t ep, void *buf, size buflen) {
    if (!buf && buflen > 0) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    channel_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int result = channel_try_recv(proc, ep, &msg);
    if (result != 0) return result;
    
    //copy data to userspace
    size to_copy = msg.data_len < buflen ? msg.data_len : buflen;
    if (to_copy > 0 && msg.data) {
        memcpy(buf, msg.data, to_copy);
    }
    
    //free the message data
    if (msg.data) kfree(msg.data);
    
    //close any transferred handles (this syscall ignores them)
    for (uint32 i = 0; i < msg.handle_count; i++) {
        process_close_handle(proc, msg.handles[i]);
    }
    if (msg.handles) kfree(msg.handles);
    
    return (int64)msg.data_len;
}

//capability-based process creation: create suspended process container
static int64 sys_process_create(const char *name) {
    if (!name) return -1;
    
    process_t *current = process_current();
    if (!current) return -1;
    
    //create a new suspended user process
    process_t *child = process_create_user_suspended(name);
    if (!child) return -1;
    
    //grant a handle to the child process back to the caller
    //this allows the caller to configure the child before starting
    int h = process_grant_handle(current, child->obj, HANDLE_RIGHTS_ALL);
    if (h < 0) {
        process_destroy(child);
        return -1;
    }
    
    return h;
}

//inject a handle from the caller into a target process
static int64 sys_handle_grant(handle_t proc_h, handle_t local_h, handle_rights_t rights) {
    process_t *current = process_current();
    if (!current) return -1;
    
    //get the target process object from the handle
    object_t *proc_obj = process_get_handle(current, proc_h);
    if (!proc_obj || proc_obj->type != OBJECT_PROCESS) return -2;
    
    process_t *target = (process_t *)proc_obj->data;
    if (!target) return -3;
    
    //get the object we want to inject
    proc_handle_t *entry = process_get_handle_entry(current, local_h);
    if (!entry) return -4;
    
    //can only grant rights we have (rights reduction)
    handle_rights_t actual_rights = rights_reduce(entry->rights, rights);
    
    //inject into target
    int new_h = process_inject_handle(target, entry->obj, actual_rights);
    return new_h;
}

//start the first thread in a suspended process
static int64 sys_process_start(handle_t proc_h, uint64 entry, uint64 stack) {
    process_t *current = process_current();
    if (!current) return -1;
    
    object_t *proc_obj = process_get_handle(current, proc_h);
    if (!proc_obj || proc_obj->type != OBJECT_PROCESS) return -2;
    
    process_t *target = (process_t *)proc_obj->data;
    if (!target) return -3;
    
    //create initial thread
    thread_t *thread = thread_create_user(target, (void *)entry, (void *)stack);
    if (!thread) return -4;
    
    //add to scheduler
    sched_add(thread);
    
    return (int64)target->pid;
}

static int64 sys_vmo_resize(handle_t vmo_h, size new_size) {
    process_t *current = process_current();
    if (!current) return -1;
    
    return vmo_resize(current, vmo_h, new_size);
}

int64 syscall_dispatch(uint64 num, uint64 arg1, uint64 arg2, uint64 arg3,
                       uint64 arg4, uint64 arg5, uint64 arg6) {
    switch (num) {
        case SYS_EXIT: return sys_exit((int64)arg1);
        case SYS_GETPID: return sys_getpid();
        case SYS_YIELD: return sys_yield();
        case SYS_DEBUG_WRITE: return sys_debug_write((const char *)arg1, (size)arg2);
        case SYS_SPAWN: return sys_spawn((const char *)arg1, (int)arg2, (char **)arg3);
        case SYS_GET_OBJ: return sys_get_obj((handle_t)arg1, (const char *)arg2, (handle_rights_t)arg3);
        case SYS_HANDLE_READ: return sys_handle_read((handle_t)arg1, (void *)arg2, (size)arg3);
        case SYS_HANDLE_WRITE: return sys_handle_write((handle_t)arg1, (const void *)arg2, (size)arg3);
        case SYS_HANDLE_SEEK: return sys_handle_seek((handle_t)arg1, (size)arg2, (int)arg3);
        case SYS_HANDLE_CLOSE: return sys_handle_close((handle_t)arg1);
        case SYS_HANDLE_DUP: return sys_handle_dup((handle_t)arg1, (handle_rights_t)arg2);
        case SYS_CHANNEL_CREATE: return sys_channel_create((int32 *)arg1, (int32 *)arg2);
        case SYS_CHANNEL_SEND: return sys_channel_send((handle_t)arg1, (const void *)arg2, (size)arg3);
        case SYS_CHANNEL_RECV: return sys_channel_recv((handle_t)arg1, (void *)arg2, (size)arg3);
        case SYS_CHANNEL_TRY_RECV: return sys_channel_try_recv((handle_t)arg1, (void *)arg2, (size)arg3);
        case SYS_VMO_CREATE: return sys_vmo_create((size)arg1, (uint32)arg2, (handle_rights_t)arg3);
        case SYS_VMO_READ: return sys_vmo_read((handle_t)arg1, (void *)arg2, (size)arg3, (size)arg4);
        case SYS_VMO_WRITE: return sys_vmo_write((handle_t)arg1, (const void *)arg2, (size)arg3, (size)arg4);
        case SYS_CHANNEL_RECV_MSG: return sys_channel_recv_msg((handle_t)arg1, (void *)arg2, (size)arg3,
                                                                (int32 *)arg4, (uint32)arg5,
                                                                (channel_recv_result_t *)arg6);
        case SYS_VMO_MAP: return sys_vmo_map((handle_t)arg1, (uintptr)arg2, (size)arg3, (size)arg4, (uint32)arg5);
        case SYS_VMO_UNMAP: return sys_vmo_unmap((uintptr)arg1, (size)arg2);
        case SYS_NS_REGISTER: return sys_ns_register((const char *)arg1, (handle_t)arg2);
        case SYS_STAT: return sys_stat((const char *)arg1, (stat_t *)arg2);
        case SYS_WAIT: return sys_wait(arg1);
        
        //capability-based process creation
        case SYS_PROCESS_CREATE: return sys_process_create((const char *)arg1);
        case SYS_HANDLE_GRANT: return sys_handle_grant((handle_t)arg1, (handle_t)arg2, (handle_rights_t)arg3);
        case SYS_PROCESS_START: return sys_process_start((handle_t)arg1, arg2, arg3);
        
        case SYS_VMO_RESIZE: return sys_vmo_resize((handle_t)arg1, (size)arg2);
        
        default: return -1;
    }
}

