#ifndef DELTA_ATOMIC_H
#define DELTA_ATOMIC_H

//project-local atomic shim
//we only need signal fences for sequencing hints in a few syscall paths
typedef enum {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5,
} memory_order;

void atomic_signal_fence(memory_order order);

#endif
