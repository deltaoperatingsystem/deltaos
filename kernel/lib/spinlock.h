#ifndef LIB_SPINLOCK_H
#define LIB_SPINLOCK_H

#include <arch/types.h>
#include <arch/cpu.h>

typedef struct {
    volatile int lock;
} spinlock_t;

#define SPINLOCK_INIT {0}

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

typedef struct {
    spinlock_t lock;
    irq_state_t irq_state;
} spinlock_irq_t;

static inline void spinlock_irq_init(spinlock_irq_t *sl) {
    spinlock_init(&sl->lock);
}

static inline void spinlock_irq_acquire(spinlock_irq_t *sl) {
    sl->irq_state = arch_irq_save();
    spinlock_acquire(&sl->lock);
}

static inline void spinlock_irq_release(spinlock_irq_t *sl) {
    spinlock_release(&sl->lock);
    arch_irq_restore(sl->irq_state);
}

#endif
