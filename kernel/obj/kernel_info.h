#ifndef OBJ_KERNEL_INFO_H
#define OBJ_KERNEL_INFO_H

#include <arch/types.h>

//category IDs for info objects
#define KERNEL_INFO_TIMER    1
#define KERNEL_INFO_MEM      2

typedef struct {
    uint32 timer_hz;
    uint32 reserved;
} kernel_info_timer_t;

typedef struct {
    uint64 total_pages;
    uint64 free_pages;
} kernel_info_mem_t;

void kernel_info_init(void);

#endif
