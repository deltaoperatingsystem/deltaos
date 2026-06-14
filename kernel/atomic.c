#include <atomic.h>

__attribute__((weak)) void atomic_signal_fence(memory_order order) {
    __atomic_signal_fence(order);
}
