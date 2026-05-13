#ifndef PROC_BOTTOM_HALF_H
#define PROC_BOTTOM_HALF_H

#include <arch/types.h>

typedef void (*bottom_half_fn_t)(void *arg);

#define BOTTOM_HALF_INVALID_HANDLE (-1)

typedef int32 bottom_half_handle_t;

void bottom_half_init(void);
bottom_half_handle_t bottom_half_register(bottom_half_fn_t fn, void *arg);
int bottom_half_unregister(bottom_half_handle_t handle);
int bottom_half_raise(bottom_half_handle_t handle);
uint32 bottom_half_run_budget(uint32 budget);

#endif
