#ifndef DRIVERS_PS2_H
#define DRIVERS_PS2_H

#include <lib/spinlock.h>

//shared spinlock for the 8042 PS/2 controller
//both keyboard and mouse share this hardware so access must be synchronized in SMP
extern spinlock_irq_t ps2_lock;

#endif
