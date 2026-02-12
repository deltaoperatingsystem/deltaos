#ifndef DRIVERS_INIT_H
#define DRIVERS_INIT_H

#include <arch/types.h>

typedef void (*driver_init_func_t)(void);

//initialization levels (sorted alphabetically by the linker)
#define INIT_LEVEL_EARLY   "0"
#define INIT_LEVEL_BUS     "1"
#define INIT_LEVEL_ARCH    "2"
#define INIT_LEVEL_DEVICE  "3"
#define INIT_LEVEL_FS      "4"
#define INIT_LEVEL_SERVICE "5"

#define DECLARE_DRIVER(func, level) \
    __attribute__((used, section(".driver_init." level))) \
    static driver_init_func_t __driver_init_entry_##func = func;

void init_drivers(void);

#endif