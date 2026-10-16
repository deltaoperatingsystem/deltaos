#ifndef LIB_SPINLOCK_H
#define LIB_SPINLOCK_H

#include <arch/types.h>
#include <arch/cpu.h>

typedef struct {
    volatile int lock;
} spinlock_t;

#define SPINLOCK_INIT {0}
#define SPINLOCK_IRQ_INIT { SPINLOCK_INIT }

static inline void spinlock_init(spinlock_t *sl) {
    sl->lock = 0;
}

static inline void spinlock_acquire(spinlock_t *sl) {
    while (__atomic_test_and_set(&sl->lock, __ATOMIC_ACQUIRE)) {
        arch_pause();
    }
}

static inline void spinlock_release(spinlock_t *sl) {
    __atomic_clear(&sl->lock, __ATOMIC_RELEASE);
}

static inline int spinlock_try_acquire(spinlock_t *sl) {
    return !__atomic_test_and_set(&sl->lock, __ATOMIC_ACQUIRE);
}

typedef struct {
    spinlock_t lock;
} spinlock_irq_t;

static inline void spinlock_irq_init(spinlock_irq_t *sl) {
    spinlock_init(&sl->lock);
}

static inline irq_state_t spinlock_irq_acquire(spinlock_irq_t *sl) {
    irq_state_t flags = arch_irq_save();
    spinlock_acquire(&sl->lock);
    return flags;
}

static inline void spinlock_irq_release(spinlock_irq_t *sl, irq_state_t flags) {
    spinlock_release(&sl->lock);
    arch_irq_restore(flags);
}

static inline int spinlock_irq_try_acquire(spinlock_irq_t *sl, irq_state_t *flags_out) {
    irq_state_t flags = arch_irq_save();
    if (spinlock_try_acquire(&sl->lock)) {
        *flags_out = flags;
        return 1;
    }
    arch_irq_restore(flags);
    return 0;
}

#endif
