#include <proc/thread.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <arch/context.h>
#include <arch/interrupts.h>
#include <mm/kheap.h>
#include <lib/string.h>

#define KERNEL_STACK_SIZE 16384  //16KB

static uint64 next_tid = 1;
static thread_t *current_thread = NULL;

//thread object ops (called when all handles to a thread are closed)
static int thread_obj_close(object_t *obj) {
    (void)obj;
    return 0;
}

static object_ops_t thread_object_ops = {
    .read = NULL,
    .write = NULL,
    .close = thread_obj_close,
    .readdir = NULL,
    .lookup = NULL
};

//kernel trampoline - enables interrupts before calling thread entry
//context switch from within ISR leaves IF=0 we enable it here so
//threads don't need to know about interrupt state
static void thread_entry_trampoline(void *thread_ptr) {
    thread_t *thread = (thread_t *)thread_ptr;
    
    //enable interrupts before calling user code
    arch_interrupts_enable();
    
    //call the actual entry function
    thread->entry(thread->arg);
    
    //thread returned so exit cleanly
    thread_exit();
}

thread_t *thread_create(process_t *proc, void (*entry)(void *), void *arg) {
    if (!proc) return NULL;
    
    thread_t *thread = kzalloc(sizeof(thread_t));
    if (!thread) return NULL;
    
    thread->tid = next_tid++;
    thread->process = proc;
    thread->state = THREAD_STATE_READY;
    
    //create kernel object for this thread
    thread->obj = object_create(OBJECT_THREAD, &thread_object_ops, thread);
    if (!thread->obj) {
        kfree(thread);
        return NULL;
    }
    
    //save entry point and arg for trampoline
    thread->entry = entry;
    thread->arg = arg;
    
    //allocate kernel stack
    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        object_deref(thread->obj);
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_size = KERNEL_STACK_SIZE;
    
    //setup initial context - trampoline will enable interrupts and call real entry
    void *stack_top = (char *)thread->kernel_stack + KERNEL_STACK_SIZE;
    arch_context_init(&thread->context, stack_top, thread_entry_trampoline, thread);
    
    //link into process thread list
    thread->next = proc->threads;
    proc->threads = thread;
    proc->thread_count++;
    
    return thread;
}

void thread_destroy(thread_t *thread) {
    if (!thread) return;
    
    //remove from process thread list
    if (thread->process) {
        thread_t **tp = &thread->process->threads;
        while (*tp) {
            if (*tp == thread) {
                *tp = thread->next;
                thread->process->thread_count--;
                break;
            }
            tp = &(*tp)->next;
        }
    }
    
    //free the thread object
    if (thread->obj) {
        thread->obj->data = NULL;  //clear back-pointer
        object_deref(thread->obj);
    }
    
    kfree(thread->kernel_stack);
    kfree(thread);
}

object_t *thread_get_object(thread_t *thread) {
    if (!thread) return NULL;
    return thread->obj;
}

thread_t *thread_current(void) {
    return current_thread;
}

void thread_set_current(thread_t *thread) {
    current_thread = thread;
}

thread_t *thread_create_user(process_t *proc, void *entry, void *user_stack) {
    if (!proc) return NULL;
    
    thread_t *thread = kzalloc(sizeof(thread_t));
    if (!thread) return NULL;
    
    thread->tid = next_tid++;
    thread->process = proc;
    thread->state = THREAD_STATE_READY;
    
    //create kernel object for this thread
    thread->obj = object_create(OBJECT_THREAD, &thread_object_ops, thread);
    if (!thread->obj) {
        kfree(thread);
        return NULL;
    }
    
    //usermode threads don't use entry/arg - context set directly
    thread->entry = NULL;
    thread->arg = NULL;
    
    //allocate kernel stack (for syscalls/interrupts)
    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        object_deref(thread->obj);
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_size = KERNEL_STACK_SIZE;
    
    //setup usermode context
    arch_context_init_user(&thread->context, user_stack, entry, NULL);
    
    //link into process thread list
    thread->next = proc->threads;
    proc->threads = thread;
    proc->thread_count++;
    
    return thread;
}

void thread_exit(void) {
    //just delegate to scheduler
    sched_exit();
}
