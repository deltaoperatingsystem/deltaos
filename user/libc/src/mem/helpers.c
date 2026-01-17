#include "internal.h"

malloc_header_t *_malloc_free_list = NULL;

malloc_header_t *_malloc_find_free_block(size desired_len) {
    malloc_header_t *curr = _malloc_free_list;
    while (curr) {
        if (curr->is_free && curr->s >= desired_len) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

void _malloc_split_block(malloc_header_t *header, size aligned_len) {
    if (header->s >= aligned_len + HEADER_SIZE + MIN_ALLOC_SIZE) {
        malloc_header_t *new_block = (malloc_header_t *)((char *)header + HEADER_SIZE + aligned_len);
        new_block->s = header->s - aligned_len - HEADER_SIZE;
        new_block->is_free = true;
        new_block->next = header->next;
        new_block->prev = header;
        
        if (header->next) header->next->prev = new_block;
        header->next = new_block;
        header->s = aligned_len;
    }
}

void _malloc_coalesce(malloc_header_t *header) {
    if (!header || !header->is_free) return;

    if (header->next && header->next->is_free) {
        header->s += HEADER_SIZE + header->next->s;
        malloc_header_t *old_next = header->next;
        header->next = old_next->next;
        if (header->next) header->next->prev = header;
    }

    if (header->prev && header->prev->is_free) {
        malloc_header_t *prev = header->prev;
        prev->s += HEADER_SIZE + header->s;
        prev->next = header->next;
        if (header->next) header->next->prev = prev;
    }
}
