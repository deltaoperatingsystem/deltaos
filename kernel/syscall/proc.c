#include <syscall/syscall.h>
#include <proc/process.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <kernel/elf64.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>
#include <obj/handle.h>

intptr sys_exit(intptr status) {
    (void)status;
    thread_exit();
    return 0;
}

intptr sys_getpid(void) {
    process_t *proc = process_current();
    if (proc) {
        return (intptr)proc->pid;
    }
    return 0;
}

intptr sys_yield(void) {
    sched_yield();
    return 0;
}

intptr sys_spawn(const char *path, int argc, char **argv) {
    //open path
    handle_t h = handle_open(path, HANDLE_RIGHT_READ);
    if (h == INVALID_HANDLE) {
        return -1;
    }
    
    //get file size
    stat_t st;
    if (handle_stat(path, &st) != 0) {
        handle_close(h);
        return -1;
    }
    
    //allocate buffer for exec binary
    size buf_size = st.size;
    #define MAX_EXEC_BUFFER (32 * 1024 * 1024) //32MB limit
    if (buf_size == 0 || buf_size > MAX_EXEC_BUFFER) {
        handle_close(h);
        return -1;
    }
    
    char *buf = kmalloc(buf_size);
    if (!buf) {
        handle_close(h);
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
    process_t *proc = process_create_user_suspended(path);
    if (!proc) {
        kfree(buf);
        return -1;
    }
    
    //inherit CWD from current process
    process_t *current = process_current();
    if (current) {
        strncpy(proc->cwd, current->cwd, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = '\0';
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
        char interp_fullpath[256];
        if (info.interp_path[0] == '/') {
            snprintf(interp_fullpath, sizeof(interp_fullpath), "$files%s", info.interp_path);
        } else {
            snprintf(interp_fullpath, sizeof(interp_fullpath), "$files/%s", info.interp_path);
        }
        
        handle_t ih = handle_open(interp_fullpath, HANDLE_RIGHT_READ);
        if (ih == INVALID_HANDLE) {
            process_destroy(proc);
            kfree(buf);
            return -1;
        }
        
        stat_t ist;
        if (handle_stat(interp_fullpath, &ist) != 0) {
            handle_close(ih);
            process_destroy(proc);
            kfree(buf);
            return -1;
        }

        size interp_buf_size = ist.size;
        if (interp_buf_size == 0 || interp_buf_size > MAX_EXEC_BUFFER) {
            handle_close(ih);
            process_destroy(proc);
            kfree(buf);
            return -1;
        }

        char *interp_buf = kmalloc(interp_buf_size);
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
        
        elf_load_info_t interp_info;
        err = elf_load_user(interp_buf, interp_len, proc, &interp_info);
        if (err != ELF_OK) {
            process_destroy(proc);
            kfree(interp_buf);
            kfree(buf);
            return -1;
        }
        
        interp_base = interp_info.virt_base;
        real_entry = interp_info.entry;
        kfree(interp_buf);
    }
    
    uintptr user_stack_base = 0x7FFFFFFFE000ULL;
    size stack_size = 0x2000;
    
    uintptr stack_phys = (uintptr)pmm_alloc(stack_size / 4096);
    if (!stack_phys) {
        return -1;
    }
    
    mmu_map_range(proc->pagemap, user_stack_base - stack_size, stack_phys, 
                  stack_size / 4096, MMU_FLAG_WRITE | MMU_FLAG_USER);
    
    process_vma_add(proc, user_stack_base - stack_size, stack_size, 
                    MMU_FLAG_WRITE | MMU_FLAG_USER, NULL, 0);
    
    uintptr user_stack_top;
    if (info.interp_path[0]) {
        user_stack_top = process_setup_user_stack_dynamic(
            stack_phys, user_stack_base, stack_size, argc, argv,
            info.phdr_addr, info.phdr_count, info.phdr_size,
            info.entry, interp_base
        );
    } else {
        user_stack_top = process_setup_user_stack(stack_phys, user_stack_base, 
                                                   stack_size, argc, argv);
    }
    
    thread_t *thread = thread_create_user(proc, (void*)real_entry, (void*)user_stack_top);
    if (!thread) {
        kfree(buf);
        return -1;
    }
    
    sched_add(thread);
    kfree(buf);
    return (intptr)proc->pid;
}

intptr sys_wait(uintptr pid) {
    process_t *proc = process_find(pid);
    if (!proc) return -1;
    
    thread_sleep(&proc->exit_wait);
    return 0;
}

intptr sys_process_create(const char *name) {
    if (!name) return -1;
    
    process_t *current = process_current();
    if (!current) return -1;
    
    process_t *child = process_create_user_suspended(name);
    if (!child) return -1;
    
    int h = process_grant_handle(current, child->obj, HANDLE_RIGHTS_ALL);
    if (h < 0) {
        process_destroy(child);
        return -1;
    }
    
    return h;
}

intptr sys_handle_grant(handle_t proc_h, handle_t local_h, handle_rights_t rights) {
    process_t *current = process_current();
    if (!current) return -1;
    
    object_t *proc_obj = process_get_handle(current, proc_h);
    if (!proc_obj || proc_obj->type != OBJECT_PROCESS) return -2;
    
    process_t *target = (process_t *)proc_obj->data;
    if (!target) return -3;
    
    proc_handle_t *entry = process_get_handle_entry(current, local_h);
    if (!entry) return -4;
    
    handle_rights_t actual_rights = rights_reduce(entry->rights, rights);
    
    int new_h = process_inject_handle(target, entry->obj, actual_rights);
    return new_h;
}

intptr sys_process_start(handle_t proc_h, uintptr entry, uintptr stack) {
    process_t *current = process_current();
    if (!current) return -1;
    
    object_t *proc_obj = process_get_handle(current, proc_h);
    if (!proc_obj || proc_obj->type != OBJECT_PROCESS) return -2;
    
    process_t *target = (process_t *)proc_obj->data;
    if (!target) return -3;
    
    thread_t *thread = thread_create_user(target, (void *)entry, (void *)stack);
    if (!thread) return -4;
    
    sched_add(thread);
    return (intptr)target->pid;
}
