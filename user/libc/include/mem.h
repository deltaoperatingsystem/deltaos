#ifndef _LIBC_MEM_H
#define _LIBC_MEM_H

#include <types.h>
#include <system.h>

#define HEAP_SIZE 1048576   // one megabyte

extern handle_t _mem_vmo;
extern void *_mem_addr;
extern size heap_capacity;

typedef struct {
    size s;
    void *addr;
} heap_blk_t;

void _mem_init(void);
void *malloc(size len);
void free(void *ptr);
void *realloc(void *ptr, size len);
void *calloc(size nmemb, size element_size);

#endif