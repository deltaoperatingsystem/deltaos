#include "internal.h"
#include <system.h>

void *malloc(size len) {
    if (len == 0) return NULL;

    size aligned_len = _malloc_align_up(len);

    if (!_mem_addr) _mem_init();

    malloc_header_t *header = _malloc_find_free_block(aligned_len);

    if (!header) {
        size grow_by = _malloc_align_up(aligned_len + HEADER_SIZE);
        if (grow_by < HEAP_SIZE) grow_by = HEAP_SIZE; 
        
        size old_cap = heap_capacity;
        size new_cap = old_cap + grow_by;

        if (vmo_resize(_mem_vmo, new_cap) == 0) {
            malloc_header_t *new_block = (malloc_header_t *)((char *)_mem_addr + old_cap);
            new_block->s = grow_by - HEADER_SIZE;
            new_block->is_free = true;
            new_block->next = NULL;
            
            malloc_header_t *last = _malloc_free_list;
            while (last->next) last = last->next;
            last->next = new_block;
            new_block->prev = last;

            heap_capacity = new_cap;
            _malloc_coalesce(new_block);
            
            header = _malloc_find_free_block(aligned_len);
            if (!header) return NULL;
        } else {
            return NULL;
        }
    }

    header->is_free = false;
    _malloc_split_block(header, aligned_len);

    return (void *)((char *)header + HEADER_SIZE);
}