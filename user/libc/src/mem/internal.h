#ifndef _LIBC_MEM_INTERNAL_H
#define _LIBC_MEM_INTERNAL_H

#include <mem.h>

typedef struct malloc_header {
    size s;                  
    bool is_free;
    struct malloc_header *next;
    struct malloc_header *prev;
} malloc_header_t;

#define MIN_ALLOC_SIZE 16
#define HEADER_SIZE sizeof(malloc_header_t)

extern malloc_header_t *_malloc_free_list;

static inline size _malloc_align_up(size s) {
    return (s + 15) & ~15;
}

static inline malloc_header_t *_malloc_get_header(void *ptr) {
    return (malloc_header_t *)((char *)ptr - HEADER_SIZE);
}

malloc_header_t *_malloc_find_free_block(size desired_len);
void _malloc_split_block(malloc_header_t *header, size aligned_len);
void _malloc_coalesce(malloc_header_t *header);

#endif
