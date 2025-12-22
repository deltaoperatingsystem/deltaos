#include <proc/thread.h>
#include <proc/process.h>
#include <arch/context.h>
#include <mm/kheap.h>
#include <lib/string.h>

#define KERNEL_STACK_SIZE 4096

static uint64 next_tid = 1;
static thread_t *current_thread = NULL;

thread_t *thread_create(process_t *proc, void (*entry)(void *), void *arg) {
    if (!proc) return NULL;
    
    thread_t *thread = kzalloc(sizeof(thread_t));
    if (!thread) return NULL;
    
    thread->tid = next_tid++;
    thread->process = proc;
    thread->state = THREAD_STATE_READY;
    
    //allocate kernel stack
    thread->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_size = KERNEL_STACK_SIZE;
    
    //setup initial context using arch-agnostic API
    void *stack_top = (char *)thread->kernel_stack + KERNEL_STACK_SIZE;
    arch_context_init(&thread->context, stack_top, entry, arg);
    
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
    
    kfree(thread->kernel_stack);
    kfree(thread);
}

thread_t *thread_current(void) {
    return current_thread;
}

void thread_set_current(thread_t *thread) {
    current_thread = thread;
}
