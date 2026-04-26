#include <syscall/syscall.h>
#include <proc/process.h>
#include <proc/event.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <kernel/elf64.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>
#include <obj/handle.h>

//strings are copied through this temporary kernel buffer limit before they are
//duplicated again into the destination process cotext
#define PROC_CONTEXT_MAX_STRING 4096
#define PROC_CONTEXT_MAX_OVERRIDES 64
#define MAX_EXEC_BUFFER (32 * 1024 * 1024)

//copy the parent baseline into the child before any one-shot spawn override is applied 
static int process_inherit_runtime_state(process_t *child, process_t *parent) {
    if (!child || !parent) return 0;

    //keep process-local startup state together so spawn and manual process
    //creation do not drift apart as ABI grows
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';

    if (proc_context_clone_for_child(&child->context, &parent->context) != 0) {
        return -1;
    }

    return 0;
}

//apply one user-provided override entry onto a freshly created child process
static int process_apply_context_override(process_t *target, process_t *source,
                                          const context_spawn_entry_t *entry) {
    char key[PROC_CONTEXT_KEY_MAX];

    if (!target || !source || !entry) return -1;
    if (copy_user_cstr(entry->key, key, sizeof(key)) != 0) return -1;

    //spawn overrides are parent-authoritative, so they may replace inherited
    //ead-only entries before the child is allowed to run
    if (proc_context_remove_force(&target->context, key) != 0) {
        proc_context_entry_t *existing = NULL;

        spinlock_acquire(&target->context.lock);
        for (size i = 0; i < target->context.count; i++) {
            if (target->context.entries[i].key &&
                strcmp(target->context.entries[i].key, key) == 0) {
                existing = &target->context.entries[i];
                break;
            }
        }
        spinlock_release(&target->context.lock);

        if (existing) return -1;
    }

    if (entry->type == CONTEXT_VALUE_STRING) {
        char *tmp;

        if (!entry->value.ptr || entry->value_len == 0 || entry->value_len > PROC_CONTEXT_MAX_STRING) {
            return -1;
        }

        tmp = kmalloc(entry->value_len);
        if (!tmp) return -1;
        if (copy_user_bytes(entry->value.ptr, tmp, entry->value_len) != 0) {
            kfree(tmp);
            return -1;
        }
        if (tmp[entry->value_len - 1] != '\0') {
            kfree(tmp);
            return -1;
        }

        int rc = proc_context_set_string(&target->context, key, tmp, entry->flags);
        kfree(tmp);
        return rc;
    }

    if (entry->type == CONTEXT_VALUE_I64) {
        return proc_context_set_i64(&target->context, key, entry->value.i64, entry->flags);
    }

    if (entry->type == CONTEXT_VALUE_U64) {
        return proc_context_set_u64(&target->context, key, entry->value.u64, entry->flags);
    }

    if (entry->type == CONTEXT_VALUE_BOOL) {
        return proc_context_set_bool(&target->context, key, entry->value.boolean != 0, entry->flags);
    }

    if (entry->type == CONTEXT_VALUE_OBJECT) {
        proc_handle_t *handle_entry = process_get_handle_entry(source, entry->value.handle);
        if (!handle_entry) return -1;
        if (!rights_has(handle_entry->rights, HANDLE_RIGHT_DUPLICATE)) return -1;

        //capture the parent's current handle rights so the child receives the
        //same capability envelope when it materializes the entry later
        return proc_context_set_object(&target->context, key, handle_entry->obj,
                                       handle_entry->rights, entry->flags);
    }

    return -1;
}

//copy the override array out of userspace one entry at a time and apply it
static int process_apply_context_overrides(process_t *target, process_t *source,
                                           const context_spawn_entry_t *entries, size entry_count) {
    if (!entries || entry_count == 0) return 0;
    if (entry_count > PROC_CONTEXT_MAX_OVERRIDES) return -1;

    for (size i = 0; i < entry_count; i++) {
        context_spawn_entry_t entry;

        if (copy_user_bytes(&entries[i], &entry, sizeof(entry)) != 0) {
            return -1;
        }

        //overrides are applied after inheritance so the parent can replace a
        //single child launch without mutating its own process context
        if (process_apply_context_override(target, source, &entry) != 0) {
            return -1;
        }
    }

    return 0;
}

//shared implementation fospawn() and spawn_ctx()
static intptr sys_spawn_impl(const char *path, int argc, char **argv,
                             const context_spawn_entry_t *entries, size entry_count) {
    if (entry_count > 0 && !entries) return -1;

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

    //inherit baseline runtime state before any one-off child override lands
    process_t *current = process_current();
    if (current && process_inherit_runtime_state(proc, current) != 0) {
        process_destroy(proc);
        kfree(buf);
        return -1;
    }
    if (current && process_apply_context_overrides(proc, current, entries, entry_count) != 0) {
        process_destroy(proc);
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

intptr sys_exit(intptr status) {
    process_exit(process_current(), (int)status);
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
    return sys_spawn_impl(path, argc, argv, NULL, 0);
}

intptr sys_spawn_ctx(const char *path, int argc, char **argv,
                     const context_spawn_entry_t *entries, size entry_count) {
    return sys_spawn_impl(path, argc, argv, entries, entry_count);
}

intptr sys_wait(uintptr pid) {
    process_t *proc = process_find_ref(pid);
    if (!proc) return -1;

    while (proc->state != PROC_STATE_ZOMBIE && proc->state != PROC_STATE_DEAD) {
        thread_sleep(&proc->exit_wait);

        process_unref(proc);
        proc = process_find_ref(pid);
        if (!proc) return -1;
    }

    int64 exit_code = proc->exit_code;
    process_destroy(proc);
    process_unref(proc);
    return exit_code;
}

intptr sys_process_create(const char *name) {
    if (!name) return -1;

    process_t *current = process_current();
    if (!current) return -1;

    process_t *child = process_create_user_suspended(name);
    if (!child) return -1;

    if (process_inherit_runtime_state(child, current) != 0) {
        process_destroy(child);
        return -1;
    }

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

intptr sys_proc_send_event(uintptr pid, uint32 event) {
    process_t *caller = process_current();
    process_t *target;
    intptr ret;

    if (!caller) return -1;
    target = process_find_ref(pid);
    if (!target) return -1;

    if (target->pid != caller->pid &&
        target->parent_pid != caller->pid &&
        caller->parent_pid != target->pid) {
        process_unref(target);
        return -1;
    }

    ret = proc_post_event(target, event);
    process_unref(target);
    return ret;
}

intptr sys_proc_set_event_handler(uint32 event, uintptr entry, uint32 flags) {
    process_t *current = process_current();
    if (!current) return -1;
    return proc_set_event_handler(current, event, entry, flags);
}

intptr sys_proc_mask_events(uint64 mask) {
    thread_t *current = thread_current();
    if (!current) return -1;
    irq_state_t flags = spinlock_irq_acquire(&current->lock);
    current->blocked_events |= (mask & PROC_EVENT_MASK_ALL);
    spinlock_irq_release(&current->lock, flags);
    return 0;
}

intptr sys_proc_unmask_events(uint64 mask) {
    thread_t *current = thread_current();
    if (!current) return -1;
    irq_state_t flags = spinlock_irq_acquire(&current->lock);
    current->blocked_events &= ~(mask & PROC_EVENT_MASK_ALL);
    spinlock_irq_release(&current->lock, flags);
    return 0;
}

intptr sys_proc_get_pending_events(uint64 *out_mask) {
    process_t *current = process_current();
    proc_event_mask_t pending;

    if (!current || !out_mask) return -1;
    if (proc_get_pending_events(current, &pending) != 0) return -1;
    return copy_to_user_bytes(out_mask, &pending, sizeof(pending));
}

intptr sys_proc_event_return(void) {
    thread_t *current = thread_current();
    if (!current) return -1;

    irq_state_t flags = spinlock_irq_acquire(&current->lock);
    if (!current->in_event_handler) {
        spinlock_irq_release(&current->lock, flags);
        return -1;
    }
    if (current->event_restore_slot != EVENT_RESTORE_KERNEL_CTX &&
        current->event_restore_slot != EVENT_RESTORE_USER_CONTEXT) {
        spinlock_irq_release(&current->lock, flags);
        return -1;
    }

    if (current->event_restore_slot == EVENT_RESTORE_KERNEL_CTX) {
        current->context = current->saved_event_context;
    }
    //syscall return always exits through user_context, including IRQ-delivered handlers
    current->user_context = current->saved_event_context;
    current->blocked_events = current->saved_blocked_events;
    current->in_event_handler = 0;
    current->event_returning = 1;
    spinlock_irq_release(&current->lock, flags);
    return 0;
}

intptr sys_proc_set_console_foreground(uintptr pid) {
    process_t *current = process_current();
    process_t *target;

    if (!current) return -1;
    if (pid != 0) {
        target = process_find_ref(pid);
        if (!target) return -1;
        if (target->pid != current->pid && target->parent_pid != current->pid) {
            process_unref(target);
            return -1;
        }
        process_unref(target);
    }
    return proc_set_console_foreground(current, pid);
}
