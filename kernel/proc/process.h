#ifndef PROC_PROCESS_H
#define PROC_PROCESS_H

#include <arch/types.h>
#include <obj/object.h>
#include <obj/rights.h>
#include <proc/wait.h>
#include <lib/spinlock.h>


//process states
#define PROC_STATE_READY    0
#define PROC_STATE_RUNNING  1
#define PROC_STATE_BLOCKED  2
#define PROC_STATE_DEAD     3
#define PROC_STATE_ZOMBIE   4

#define PROC_INITIAL_HANDLES 16

struct thread;

//per-process handle entry (capability)
typedef struct {
    object_t *obj;              //the kernel object
    size offset;                //file position (for seekable objects)
    uint32 flags;               //open flags (O_RDONLY, etc.)
    handle_rights_t rights;     //capability rights
} proc_handle_t;

//virtual memory area (for tracking user mappings)
typedef struct proc_vma {
    uintptr start;              //start virtual address
    size length;                //length in bytes
    uint32 flags;               //mapping flags
    object_t *obj;              //backing object (VMO) if any
    size obj_offset;            //offset into backing object
    struct proc_vma *next;      //linked list
} proc_vma_t;


//process structure
typedef struct process {
    uint64 pid;
    uint64 parent_pid;  //parent process ID (0 for init/kernel)
    char name[32];
    char cwd[256];  //current working directory
    uint32 state;
    
    //kernel object wrapper (for capability-based access)
    object_t *obj;
    
    //capability-based handle table (dynamic)
    proc_handle_t *handles;
    uint32 handle_count;
    uint32 handle_capacity;
    
    //address space (NULL for kernel threads)
    void *pagemap;
    
    //virtual memory areas (for address space tracking)
    proc_vma_t *vma_list;       //head of VMA list
    uintptr vma_next_addr;      //next allocation address hint
    
    //threads in this process
    struct thread *threads;
    uint32 thread_count;
    
    int64 exit_code;
    wait_queue_t exit_wait;
    
    //linked list for scheduler
    struct process *next;

    spinlock_t lock;
} process_t;

//user address space bounds
#define USER_SPACE_START    0x0000000000400000ULL  //4MB
#define USER_SPACE_END      0x00007FFFFFFFFFFFULL  //canonical low half

//find a process by PID
process_t *process_find(uint64 pid);

//create a new process
process_t *process_create(const char *name);

//create a new userspace process (with address space)
process_t *process_create_user(const char *name);

//create a new userspace process in suspended state
process_t *process_create_user_suspended(const char *name);

//destroy a process
void process_destroy(process_t *proc);

//get the process as a kernel object (for granting handles to processes)
object_t *process_get_object(process_t *proc);

//grant a handle to a process with rights (returns handle index or -1)
int process_grant_handle(process_t *proc, object_t *obj, handle_rights_t rights);

//inject a handle from a source process into a target process (returns new handle index)
//this is how a parent configures its child's capabilities
int process_inject_handle(process_t *target, object_t *obj, handle_rights_t rights);

//get object from handle (does NOT add ref)
object_t *process_get_handle(process_t *proc, int handle);

//get handle entry (for rights/offset access)
proc_handle_t *process_get_handle_entry(process_t *proc, int handle);

//check if handle has required rights
int process_handle_has_rights(process_t *proc, int handle, handle_rights_t required);

//duplicate a handle with same or reduced rights (returns new handle or -1)
int process_duplicate_handle(process_t *proc, int handle, handle_rights_t new_rights);

//replace handle rights (can only reduce, never increase)
int process_replace_handle_rights(process_t *proc, int handle, handle_rights_t new_rights);

//close a process handle
int process_close_handle(process_t *proc, int handle);

//get current process
process_t *process_current(void);

//set current process
void process_set_current(process_t *proc);

//get the kernel process (PID 0) as it owns kernel-internal handles
process_t *process_get_kernel(void);

//initialize process system with kernel process 0
void proc_init(void);

//allocate a free virtual address region of given size
//returns start address or 0 on failure
uintptr process_vma_alloc(process_t *proc, size length, uint32 flags, object_t *backing_obj, size obj_offset);

//find free virtual address region
uintptr process_vma_find_free(process_t *proc, size length);

//add a VMA entry (for tracking existing mappings)
int process_vma_add(process_t *proc, uintptr start, size length, uint32 flags, object_t *backing_obj, size obj_offset);

//remove a VMA entry
int process_vma_remove(process_t *proc, uintptr start);

//find VMA containing the given address
proc_vma_t *process_vma_find(process_t *proc, uintptr addr);

//setup user stack with argc/argv
//returns adjusted stack pointer to use for thread creation
uintptr process_setup_user_stack(uintptr stack_phys, uintptr stack_base,
                                  size stack_size, int argc, char *argv[]);

//aux vector entry types (for dynamic linker)
#define AT_NULL     0   //end of aux vector
#define AT_PHDR     3   //program headers address
#define AT_PHENT    4   //size of program header entry
#define AT_PHNUM    5   //number of program headers
#define AT_PAGESZ   6   //system page size
#define AT_BASE     7   //interpreter base address
#define AT_ENTRY    9   //program entry point
#define AT_RANDOM   25  //address of random bytes

//aux vector entry
typedef struct {
    uint64 a_type;
    uint64 a_val;
} auxv_entry_t;

//setup user stack with argc/argv and aux vector (for dynamic executables)
//interp_base: where interpreter was loaded (0 if no interpreter)
uintptr process_setup_user_stack_dynamic(uintptr stack_phys, uintptr stack_base,
                                          size stack_size, int argc, char *argv[],
                                          uint64 phdr_addr, uint16 phdr_count, uint16 phdr_size,
                                          uint64 entry_point, uint64 interp_base);

void process_iterate(void (*cb)(process_t *proc, void *data), void *data);

#endif
