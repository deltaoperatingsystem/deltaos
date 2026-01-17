#ifndef _MEM_H
#define _MEM_H

#include <types.h>
#include <system.h>

#define HEAP_SIZE 1048576   // one megabyte

extern handle_t _mem_vmo;
extern void *_mem_addr;

typedef struct {
    size size;
    void *addr;
} heap_blk_t;

void *malloc(size size);

#endif