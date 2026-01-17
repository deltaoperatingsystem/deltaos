#include "internal.h"
#include <string.h>

void *realloc(void *ptr, size len) {
    if (!ptr) return malloc(len);
    if (len == 0) {
        free(ptr);
        return NULL;
    }

    malloc_header_t *header = _malloc_get_header(ptr);
    size aligned_len = _malloc_align_up(len);

    if (header->s >= aligned_len) {
        _malloc_split_block(header, aligned_len);
        return ptr;
    }

    if (header->next && header->next->is_free && (header->s + HEADER_SIZE + header->next->s) >= aligned_len) {
        header->s += HEADER_SIZE + header->next->s;
        malloc_header_t *old_next = header->next;
        header->next = old_next->next;
        if (header->next) header->next->prev = header;
        
        _malloc_split_block(header, aligned_len);
        return ptr;
    }

    void *new_ptr = malloc(len);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, header->s);
    free(ptr);
    return new_ptr;
}
