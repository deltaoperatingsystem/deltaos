#include <proc/process.h>
#include <proc/thread.h>
#include <mm/kheap.h>
#include <mm/mm.h>
#include <arch/mmu.h>
#include <lib/string.h>
#include <lib/io.h>

static uint64 next_pid = 1;
static process_t *process_list = NULL;
static process_t *current_process = NULL;
static process_t *kernel_process = NULL;

//process object ops (called when all handles to a process are closed)
static int process_obj_close(object_t *obj) {
    (void)obj;
    return 0;
}

static object_ops_t process_object_ops = {
    .read = NULL,
    .write = NULL,
    .close = process_obj_close,
    .ioctl = NULL,
    .readdir = NULL
};

process_t *process_create(const char *name) {
    process_t *proc = kzalloc(sizeof(process_t));
    if (!proc) return NULL;
    
    proc->pid = next_pid++;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->state = PROC_STATE_READY;
    
    //create kernel object for this process
    proc->obj = object_create(OBJECT_PROCESS, &process_object_ops, proc);
    if (!proc->obj) {
        kfree(proc);
        return NULL;
    }
    
    //allocate dynamic handle table
    proc->handles = kzalloc(PROC_INITIAL_HANDLES * sizeof(proc_handle_t));
    if (!proc->handles) {
        object_deref(proc->obj);
        kfree(proc);
        return NULL;
    }
    proc->handle_count = 0;
    proc->handle_capacity = PROC_INITIAL_HANDLES;
    
    proc->pagemap = NULL;
    proc->threads = NULL;
    proc->thread_count = 0;
    
    //add to process list
    proc->next = process_list;
    process_list = proc;
    
    return proc;
}

process_t *process_create_user(const char *name) {
    process_t *proc = process_create(name);
    if (!proc) return NULL;
    
    //create user address space
    proc->pagemap = mmu_pagemap_create();
    if (!proc->pagemap) {
        process_destroy(proc);
        return NULL;
    }
    
    return proc;
}

void process_destroy(process_t *proc) {
    if (!proc) return;
    
    //close all handles
    for (uint32 i = 0; i < proc->handle_capacity; i++) {
        if (proc->handles[i].obj) {
            object_deref(proc->handles[i].obj);
        }
    }
    kfree(proc->handles);
    
    //free user address space if present
    if (proc->pagemap) {
        mmu_pagemap_destroy(proc->pagemap);
        proc->pagemap = NULL;
    }
    
    //remove from process list
    process_t **pp = &process_list;
    while (*pp) {
        if (*pp == proc) {
            *pp = proc->next;
            break;
        }
        pp = &(*pp)->next;
    }
    
    //free the process object
    if (proc->obj) {
        proc->obj->data = NULL; //clear back-pointer
        object_deref(proc->obj);
    }
    
    kfree(proc);
}

object_t *process_get_object(process_t *proc) {
    if (!proc) return NULL;
    return proc->obj;
}

int process_grant_handle(process_t *proc, object_t *obj, handle_rights_t rights) {
    if (!proc || !obj) return -1;
    
    //find free slot
    for (uint32 i = 0; i < proc->handle_capacity; i++) {
        if (!proc->handles[i].obj) {
            proc->handles[i].obj = obj;
            proc->handles[i].offset = 0;
            proc->handles[i].flags = 0;
            proc->handles[i].rights = rights;
            object_ref(obj);
            proc->handle_count++;
            return i;
        }
    }
    
    //grow table
    uint32 new_cap = proc->handle_capacity * 2;
    proc_handle_t *new_handles = krealloc(proc->handles, new_cap * sizeof(proc_handle_t));
    if (!new_handles) return -1;
    
    //zero new entries
    for (uint32 i = proc->handle_capacity; i < new_cap; i++) {
        new_handles[i].obj = NULL;
        new_handles[i].offset = 0;
        new_handles[i].flags = 0;
        new_handles[i].rights = HANDLE_RIGHT_NONE;
    }
    
    int h = proc->handle_capacity;
    new_handles[h].obj = obj;
    new_handles[h].offset = 0;
    new_handles[h].flags = 0;
    new_handles[h].rights = rights;
    object_ref(obj);
    
    proc->handles = new_handles;
    proc->handle_capacity = new_cap;
    proc->handle_count++;
    
    return h;
}

object_t *process_get_handle(process_t *proc, int handle) {
    if (!proc || handle < 0 || (uint32)handle >= proc->handle_capacity) return NULL;
    return proc->handles[handle].obj;
}

proc_handle_t *process_get_handle_entry(process_t *proc, int handle) {
    if (!proc || handle < 0 || (uint32)handle >= proc->handle_capacity) return NULL;
    if (!proc->handles[handle].obj) return NULL;
    return &proc->handles[handle];
}

int process_handle_has_rights(process_t *proc, int handle, handle_rights_t required) {
    proc_handle_t *entry = process_get_handle_entry(proc, handle);
    if (!entry) return 0;
    return rights_has(entry->rights, required);
}

int process_duplicate_handle(process_t *proc, int handle, handle_rights_t new_rights) {
    proc_handle_t *entry = process_get_handle_entry(proc, handle);
    if (!entry) return -1;
    
    //check DUPLICATE right
    if (!rights_has(entry->rights, HANDLE_RIGHT_DUPLICATE)) {
        return -1;  //not allowed to duplicate
    }
    
    //new rights must be subset of original (can only reduce never increase)
    handle_rights_t actual_rights = rights_reduce(entry->rights, new_rights);
    
    //create new handle with reduced rights
    return process_grant_handle(proc, entry->obj, actual_rights);
}

int process_replace_handle_rights(process_t *proc, int handle, handle_rights_t new_rights) {
    proc_handle_t *entry = process_get_handle_entry(proc, handle);
    if (!entry) return -1;
    
    //can only reduce rights never increase
    if ((new_rights & ~entry->rights) != 0) {
        return -1;  //trying to add rights we don't have
    }
    
    entry->rights = new_rights;
    return 0;
}

int process_close_handle(process_t *proc, int handle) {
    if (!proc || handle < 0 || (uint32)handle >= proc->handle_capacity) return -1;
    if (!proc->handles[handle].obj) return -1;
    
    object_deref(proc->handles[handle].obj);
    proc->handles[handle].obj = NULL;
    proc->handles[handle].offset = 0;
    proc->handles[handle].flags = 0;
    proc->handles[handle].rights = HANDLE_RIGHT_NONE;
    proc->handle_count--;
    
    return 0;
}

process_t *process_current(void) {
    return current_process;
}

void process_set_current(process_t *proc) {
    current_process = proc;
}

process_t *process_get_kernel(void) {
    return kernel_process;
}

void proc_init(void) {
    //create kernel process (PID 0)
    process_t *kproc = process_create("kernel");
    if (!kproc) {
        printf("[proc] ERR: failed to create kernel process\n");
        return;
    }
    
    //kernel process is PID 0 so adjust
    kproc->pid = 0;
    next_pid = 1;  //next process will be PID 1
    
    kproc->state = PROC_STATE_RUNNING;
    kernel_process = kproc;
    process_set_current(kproc);
    
    printf("[proc] initialized (kernel PID 0)\n");
}

uintptr process_vma_find_free(process_t *proc, size length) {
    if (!proc || length == 0) return 0;
    
    //page-align the length
    length = (length + 0xFFF) & ~0xFFFULL;
    
    //start from the hint or default
    uintptr addr = proc->vma_next_addr;
    if (addr < USER_SPACE_START) addr = USER_SPACE_START;
    
    //scan for a free region
    while (addr + length <= USER_SPACE_END) {
        int conflict = 0;
        
        //check against all existing VMAs
        for (proc_vma_t *vma = proc->vma_list; vma; vma = vma->next) {
            uintptr vma_end = vma->start + vma->length;
            uintptr region_end = addr + length;
            
            //check for overlap
            if (addr < vma_end && region_end > vma->start) {
                //overlap - skip past this VMA
                addr = vma_end;
                addr = (addr + 0xFFF) & ~0xFFFULL;  //page align
                conflict = 1;
                break;
            }
        }
        
        if (!conflict) {
            //found a free region
            proc->vma_next_addr = addr + length;
            return addr;
        }
    }
    
    return 0;  //no space found
}

int process_vma_add(process_t *proc, uintptr start, size length, 
                    uint32 flags, object_t *backing_obj, size obj_offset) {
    if (!proc) return -1;
    
    proc_vma_t *vma = kzalloc(sizeof(proc_vma_t));
    if (!vma) return -1;
    
    vma->start = start;
    vma->length = length;
    vma->flags = flags;
    vma->obj = backing_obj;
    vma->obj_offset = obj_offset;
    
    if (backing_obj) object_ref(backing_obj);
    
    //insert at head of list
    vma->next = proc->vma_list;
    proc->vma_list = vma;
    
    return 0;
}

uintptr process_vma_alloc(process_t *proc, size length, uint32 flags,
                          object_t *backing_obj, size obj_offset) {
    uintptr addr = process_vma_find_free(proc, length);
    if (!addr) return 0;
    
    if (process_vma_add(proc, addr, (length + 0xFFF) & ~0xFFFULL, 
                        flags, backing_obj, obj_offset) < 0) {
        return 0;
    }
    
    return addr;
}

int process_vma_remove(process_t *proc, uintptr start) {
    if (!proc) return -1;
    
    proc_vma_t **pp = &proc->vma_list;
    while (*pp) {
        if ((*pp)->start == start) {
            proc_vma_t *vma = *pp;
            *pp = vma->next;
            
            if (vma->obj) object_deref(vma->obj);
            kfree(vma);
            return 0;
        }
        pp = &(*pp)->next;
    }
    
    return -1;  //not found
}

proc_vma_t *process_vma_find(process_t *proc, uintptr addr) {
    if (!proc) return NULL;
    
    for (proc_vma_t *vma = proc->vma_list; vma; vma = vma->next) {
        if (addr >= vma->start && addr < vma->start + vma->length) {
            return vma;
        }
    }
    
    return NULL;
}

uintptr process_setup_user_stack(uintptr stack_phys, uintptr stack_base, 
                                  size stack_size, int argc, char *argv[]) {
    //write to physical memory since user pagemap isn't active
    char *stack_virt = (char *)P2V(stack_phys);
    uintptr offset_from_base = stack_base - stack_size;
    uintptr sp = stack_base;
    
    //first pass: calculate total space needed for strings and store addresses
    uintptr argv_addrs[64];  //max 64 args
    if (argc > 64) argc = 64;
    
    //write argv strings at top of stack (reverse order for natural layout)
    for (int i = argc - 1; i >= 0; i--) {
        size len = strlen(argv[i]) + 1;
        sp -= len;
        sp &= ~7ULL;  //8-byte align
        argv_addrs[i] = sp;
        memcpy(stack_virt + (sp - offset_from_base), argv[i], len);
    }
    
    //push NULL terminator for argv
    sp -= 8;
    *(uint64 *)(stack_virt + (sp - offset_from_base)) = 0;
    
    //push argv pointers in reverse order
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64 *)(stack_virt + (sp - offset_from_base)) = argv_addrs[i];
    }
    
    //push argc
    sp -= 8;
    *(uint64 *)(stack_virt + (sp - offset_from_base)) = (uint64)argc;
    
    return sp;
}
