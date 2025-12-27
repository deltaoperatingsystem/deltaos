#ifndef PROC_PROCESS_H
#define PROC_PROCESS_H

#include <arch/types.h>
#include <obj/object.h>

//process states
#define PROC_STATE_READY    0
#define PROC_STATE_RUNNING  1
#define PROC_STATE_BLOCKED  2
#define PROC_STATE_DEAD     3

#define PROC_INITIAL_HANDLES 16

struct thread;

//per-process handle entry
typedef struct {
    object_t *obj;
    size offset;
    uint32 flags;
} proc_handle_t;

//process structure
typedef struct process {
    uint64 pid;
    char name[32];
    uint32 state;
    
    //capability-based handle table (dynamic)
    proc_handle_t *handles;
    uint32 handle_count;
    uint32 handle_capacity;
    
    //address space (NULL for kernel threads)
    void *pagemap;
    
    //threads in this process
    struct thread *threads;
    uint32 thread_count;
    
    //linked list for scheduler
    struct process *next;
} process_t;

//create a new process
process_t *process_create(const char *name);

//create a new userspace process (with address space)
process_t *process_create_user(const char *name);

//destroy a process
void process_destroy(process_t *proc);

//grant a handle to a process (returns handle index or -1)
int process_grant_handle(process_t *proc, object_t *obj, uint32 flags);

//get object from process handle
object_t *process_get_handle(process_t *proc, int handle);

//close a process handle
int process_close_handle(process_t *proc, int handle);

//get current process
process_t *process_current(void);

//set current process
void process_set_current(process_t *proc);

//initialize process system with kernel process 0
void proc_init(void);

#endif
