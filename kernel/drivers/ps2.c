#include <drivers/ps2.h>

//shared lock for the 8042 controller
spinlock_irq_t ps2_lock = SPINLOCK_IRQ_INIT;
