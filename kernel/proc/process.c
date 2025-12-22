#include <proc/process.h>
#include <proc/thread.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>

static uint64 next_pid = 1;
static process_t *process_list = NULL;
static process_t *current_process = NULL;

process_t *process_create(const char *name) {
    process_t *proc = kzalloc(sizeof(process_t));
    if (!proc) return NULL;
    
    proc->pid = next_pid++;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->state = PROC_STATE_READY;
    
    //allocate dynamic handle table
    proc->handles = kzalloc(PROC_INITIAL_HANDLES * sizeof(proc_handle_t));
    if (!proc->handles) {
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

void process_destroy(process_t *proc) {
    if (!proc) return;
    
    //close all handles
    for (uint32 i = 0; i < proc->handle_capacity; i++) {
        if (proc->handles[i].obj) {
            object_deref(proc->handles[i].obj);
        }
    }
    kfree(proc->handles);
    
    //remove from process list
    process_t **pp = &process_list;
    while (*pp) {
        if (*pp == proc) {
            *pp = proc->next;
            break;
        }
        pp = &(*pp)->next;
    }
    
    kfree(proc);
}

int process_grant_handle(process_t *proc, object_t *obj, uint32 flags) {
    if (!proc || !obj) return -1;
    
    //find free slot
    for (uint32 i = 0; i < proc->handle_capacity; i++) {
        if (!proc->handles[i].obj) {
            proc->handles[i].obj = obj;
            proc->handles[i].offset = 0;
            proc->handles[i].flags = flags;
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
    }
    
    int h = proc->handle_capacity;
    new_handles[h].obj = obj;
    new_handles[h].offset = 0;
    new_handles[h].flags = flags;
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

int process_close_handle(process_t *proc, int handle) {
    if (!proc || handle < 0 || (uint32)handle >= proc->handle_capacity) return -1;
    if (!proc->handles[handle].obj) return -1;
    
    object_deref(proc->handles[handle].obj);
    proc->handles[handle].obj = NULL;
    proc->handles[handle].offset = 0;
    proc->handles[handle].flags = 0;
    proc->handle_count--;
    
    return 0;
}

process_t *process_current(void) {
    return current_process;
}

void process_set_current(process_t *proc) {
    current_process = proc;
}

void proc_init(void) {
    //create kernel process (PID 0)
    process_t *kernel_proc = process_create("kernel");
    if (!kernel_proc) {
        printf("[proc] ERR: failed to create kernel process\n");
        return;
    }
    
    //kernel process is PID 0, so adjust
    kernel_proc->pid = 0;
    next_pid = 1;  //next process will be PID 1
    
    kernel_proc->state = PROC_STATE_RUNNING;
    process_set_current(kernel_proc);
    
    printf("[proc] initialized (kernel PID 0)\n");
}

