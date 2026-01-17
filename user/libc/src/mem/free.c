#include "internal.h"

void free(void *ptr) {
    if (!ptr) return;

    malloc_header_t *header = _malloc_get_header(ptr);
    header->is_free = true;
    _malloc_coalesce(header);
}
