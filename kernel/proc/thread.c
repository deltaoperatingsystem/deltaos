#include <proc/thread.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <arch/context.h>
#include <arch/interrupts.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/spinlock.h>
#include <arch/percpu.h>

#define KERNEL_STACK_SIZE 16384  //16KB

static uint64 next_tid = 1;
static spinlock_t tid_lock = SPINLOCK_INIT;

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
    
    spinlock_acquire(&tid_lock);
    thread->tid = next_tid++;
    spinlock_release(&tid_lock);
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
    
    spinlock_acquire(&proc->lock);
    //link into process thread list
    thread->next = proc->threads;
    proc->threads = thread;
    proc->thread_count++;
    spinlock_release(&proc->lock);
    
    return thread;
}

void thread_destroy(thread_t *thread) {
    if (!thread) return;
    
    process_t *proc = thread->process;
    
    //remove from process thread list
    if (proc) {
        spinlock_acquire(&proc->lock);
        thread_t **tp = &proc->threads;
        while (*tp) {
            if (*tp == thread) {
                *tp = thread->next;
                proc->thread_count--;
                break;
            }
            tp = &(*tp)->next;
        }
        spinlock_release(&proc->lock);
    }
    
    //free the thread object
    if (thread->obj) {
        thread->obj->data = NULL;  //clear back-pointer
        object_deref(thread->obj);
    }
    
    kfree(thread->kernel_stack);
    kfree(thread);
    
    //if this was the last thread, destroy the process too
    if (proc && proc->thread_count == 0 && proc->pid != 0) {
        process_destroy(proc);
    }
}

object_t *thread_get_object(thread_t *thread) {
    if (!thread) return NULL;
    return thread->obj;
}

thread_t *thread_current(void) {
    percpu_t *pc = percpu_get();
    return (thread_t *)pc->current_thread;
}

void thread_set_current(thread_t *thread) {
    percpu_t *pc = percpu_get();
    pc->current_thread = thread;
}

static void user_thread_trampoline(void *thread_ptr) {
    thread_t *thread = (thread_t *)thread_ptr;
    
    //enable interrupts (arch_context_switch doesn't restore IF)
    arch_interrupts_enable();
    
    //transition to usermode
    arch_enter_usermode(&thread->user_context);
    
    //never reached
    thread_exit();
}

thread_t *thread_create_user(process_t *proc, void *entry, void *user_stack) {
    if (!proc) return NULL;
    
    thread_t *thread = kzalloc(sizeof(thread_t));
    if (!thread) return NULL;
    
    spinlock_acquire(&tid_lock);
    thread->tid = next_tid++;
    spinlock_release(&tid_lock);
    thread->process = proc;
    thread->state = THREAD_STATE_READY;
    
    //create kernel object for this thread
    thread->obj = object_create(OBJECT_THREAD, &thread_object_ops, thread);
    if (!thread->obj) {
        kfree(thread);
        return NULL;
    }
    
    //usermode threads use their own user_context for iretq
    thread->entry = NULL;
    thread->arg = NULL;
    
    //allocate kernel stack (for syscalls/interrupts/trampoline)
    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        object_deref(thread->obj);
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_size = KERNEL_STACK_SIZE;
    
    //setup usermode state in user_context
    arch_context_init_user(&thread->user_context, user_stack, entry, NULL);
    
    //setup initial KERNEL context to run the trampoline
    void *stack_top = (char *)thread->kernel_stack + KERNEL_STACK_SIZE;
    arch_context_init(&thread->context, stack_top, user_thread_trampoline, thread);
    
    spinlock_acquire(&proc->lock);
    //link into process thread list
    thread->next = proc->threads;
    proc->threads = thread;
    proc->thread_count++;
    spinlock_release(&proc->lock);
    
    return thread;
}

void thread_exit(void) {
    //just delegate to scheduler
    sched_exit();
}
