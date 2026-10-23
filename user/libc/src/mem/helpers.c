#include "internal.h"

malloc_header_t *_malloc_free_list = NULL;
malloc_header_t *_malloc_tail = NULL;

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
        else _malloc_tail = new_block;
        header->next = new_block;
        header->s = aligned_len;
    }
}

//merges a free block with free neighbours and returns the block that survives;
//the passed header must not be used afterwards, as it may have merged into its predecessor
malloc_header_t *_malloc_coalesce(malloc_header_t *header) {
    if (!header || !header->is_free) return header;

    if (header->next && header->next->is_free) {
        malloc_header_t *old_next = header->next;
        header->s += HEADER_SIZE + old_next->s;
        old_next->is_free = false;
        header->next = old_next->next;
        if (header->next) header->next->prev = header;
        else _malloc_tail = header;
    }

    if (header->prev && header->prev->is_free) {
        malloc_header_t *prev = header->prev;
        prev->s += HEADER_SIZE + header->s;
        header->is_free = false;
        prev->next = header->next;
        if (header->next) header->next->prev = prev;
        else _malloc_tail = prev;
        return prev;
    }

    return header;
}
