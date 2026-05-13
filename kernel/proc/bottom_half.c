//drivers register a callback they can raise from interrupt context
//raising just queues it, the actual work runs later from a safer context
//(typically a scheduler tick) via bottom_half_run_budget() T
// so this lets IRQ
//handlers stay short by deferring anything that would touch the heap take
//long locks, or do significant work

#include <proc/bottom_half.h>
#include <lib/spinlock.h>
#include <mm/kheap.h>

//single global lock guarding both lists below
//should be acquired with IRQs disabled
//because raise() is called from interrupt context
static spinlock_irq_t bottom_half_lock = SPINLOCK_IRQ_INIT;

//each registered callback gets one of these. It lives on two singly-linked
//lists at the same time:
//  next_all     -> the master list of every registered handler (lookup/unregister)
//  next_pending -> the FIFO of handlers raised and waiting to run
typedef struct bottom_half_entry {
    bottom_half_handle_t handle;
    bottom_half_fn_t fn;
    void *arg;
    uint8 queued;       //1 if currently on the pending list
    struct bottom_half_entry *next_all;
    struct bottom_half_entry *next_pending;
} bottom_half_entry_t;

static bottom_half_entry_t *bottom_half_entries = NULL;        //master list
static bottom_half_entry_t *bottom_half_pending_head = NULL;   //FIFO head
static bottom_half_entry_t *bottom_half_pending_tail = NULL;   //FIFO tail
static bottom_half_handle_t bottom_half_next_handle = 1;       //handle allocator

//walk the master list and find the entry matching this handle.
//caller must hold bottom_half_lock.
static bottom_half_entry_t *bottom_half_find_locked(bottom_half_handle_t handle) {
    bottom_half_entry_t *entry;

    for (entry = bottom_half_entries; entry != NULL; entry = entry->next_all) {
        if (entry->handle == handle) {
            return entry;
        }
    }

    return NULL;
}

//reset all global state. Called once during early boot before any driver
//can register a bottom half.
void bottom_half_init(void) {
    bottom_half_entries = NULL;
    bottom_half_pending_head = NULL;
    bottom_half_pending_tail = NULL;
    bottom_half_next_handle = 1;
    spinlock_irq_init(&bottom_half_lock);
}

//register a callback and return an opaque handle the driver uses later to
//raise (queue) it. Returns BOTTOM_HALF_INVALID_HANDLE on allocation failure
//or if the handle counter has overflowed.
bottom_half_handle_t bottom_half_register(bottom_half_fn_t fn, void *arg) {
    bottom_half_entry_t *entry;
    irq_state_t flags;

    if (!fn) return BOTTOM_HALF_INVALID_HANDLE;

    //allocate outside the lock to keep the critical section short
    entry = kzalloc(sizeof(*entry));
    if (!entry) return BOTTOM_HALF_INVALID_HANDLE;

    flags = spinlock_irq_acquire(&bottom_half_lock);

    //defensive overflow check; with int32 handles this is not realistic
    //but it costs nothing to verify
    if (bottom_half_next_handle <= 0) {
        spinlock_irq_release(&bottom_half_lock, flags);
        kfree(entry);
        return BOTTOM_HALF_INVALID_HANDLE;
    }

    entry->handle = bottom_half_next_handle++;
    entry->fn = fn;
    entry->arg = arg;
    entry->queued = 0;
    entry->next_all = bottom_half_entries;     //push onto the master list
    entry->next_pending = NULL;
    bottom_half_entries = entry;

    spinlock_irq_release(&bottom_half_lock, flags);
    return entry->handle;
}

//remove a registered handler. Also yanks it out of the pending queue if
//it happens to be there, so an in-flight raise won't fire after unregister.
int bottom_half_unregister(bottom_half_handle_t handle) {
    bottom_half_entry_t *prev_all;
    bottom_half_entry_t *entry;
    bottom_half_entry_t *prev_pending;
    irq_state_t flags;

    if (handle == BOTTOM_HALF_INVALID_HANDLE) return -1;

    flags = spinlock_irq_acquire(&bottom_half_lock);

    //locate the entry on the master list, remembering its predecessor so
    //we can splice it out below
    prev_all = NULL;
    entry = bottom_half_entries;
    while (entry != NULL && entry->handle != handle) {
        prev_all = entry;
        entry = entry->next_all;
    }

    if (entry == NULL) {
        spinlock_irq_release(&bottom_half_lock, flags);
        return -1;
    }

    //if it's currently queued, walk the pending list and unlink it.
    //we have to update the tail pointer if we removed the last node
    prev_pending = NULL;
    for (bottom_half_entry_t *cur = bottom_half_pending_head; cur != NULL; cur = cur->next_pending) {
        if (cur == entry) {
            if (prev_pending != NULL) {
                prev_pending->next_pending = cur->next_pending;
            } else {
                bottom_half_pending_head = cur->next_pending;
            }

            if (bottom_half_pending_tail == cur) {
                bottom_half_pending_tail = prev_pending;
            }

            cur->queued = 0;
            cur->next_pending = NULL;
            break;
        }

        prev_pending = cur;
    }

    //splice out of the master list
    if (prev_all != NULL) {
        prev_all->next_all = entry->next_all;
    } else {
        bottom_half_entries = entry->next_all;
    }

    spinlock_irq_release(&bottom_half_lock, flags);
    kfree(entry);
    return 0;
}

//queue a registered handler to run later
//safe to call from interrupt contex,r epeated raises before the handler
//runs are coalesced into one (the `queued` flag ensures we don't add the
//entry to the pending list twice), so a flood of IRQs won't grow the
//queue unboundedly
int bottom_half_raise(bottom_half_handle_t handle) {
    bottom_half_entry_t *entry;
    irq_state_t flags;

    if (handle == BOTTOM_HALF_INVALID_HANDLE) return -1;

    flags = spinlock_irq_acquire(&bottom_half_lock);
    entry = bottom_half_find_locked(handle);
    if (entry == NULL || entry->fn == NULL) {
        spinlock_irq_release(&bottom_half_lock, flags);
        return -1;
    }

    //coalesce repeated raises until the handler has had one chance to run
    if (!entry->queued) {
        entry->queued = 1;
        entry->next_pending = NULL;
        if (bottom_half_pending_tail != NULL) {
            bottom_half_pending_tail->next_pending = entry;
        } else {
            bottom_half_pending_head = entry;
        }
        bottom_half_pending_tail = entry;
    }

    spinlock_irq_release(&bottom_half_lock, flags);
    return 0;
}

//drain pending bottom halves, running up to `budget` of them
//`budget == 0` means  to run them all
//returns the number actually run
uint32 bottom_half_run_budget(uint32 budget) {
    uint32 ran = 0;

    while (budget == 0 || ran < budget) {
        bottom_half_entry_t *entry;
        bottom_half_fn_t fn;
        void *arg;
        irq_state_t flags;

        flags = spinlock_irq_acquire(&bottom_half_lock);
        entry = bottom_half_pending_head;
        if (entry == NULL) {
            spinlock_irq_release(&bottom_half_lock, flags);
            break;
        }

        //pop from the head of the FIFO and clear the queued flag so the
        //handler is free to re-raise itself during execution
        bottom_half_pending_head = entry->next_pending;
        if (bottom_half_pending_head == NULL) {
            bottom_half_pending_tail = NULL;
        }
        entry->queued = 0;
        entry->next_pending = NULL;
        fn = entry->fn;
        arg = entry->arg;
        spinlock_irq_release(&bottom_half_lock, flags);

        //fn could be NULL if the entry was being unregistered concurrently
        //skip without counting it against the budget
        if (!fn) {
            continue;
        }

        fn(arg);
        ++ran;
    }

    return ran;
}
