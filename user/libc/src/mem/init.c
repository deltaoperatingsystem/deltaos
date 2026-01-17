#include <system.h>
#include <mem.h>
#include "internal.h"

handle_t _mem_vmo = INVALID_HANDLE;
void *_mem_addr = 0;
size heap_capacity = 0;

void _mem_init(void) {
    if (_mem_addr) return;

    _mem_vmo = vmo_create(HEAP_SIZE, VMO_FLAG_RESIZABLE, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    _mem_addr = vmo_map(_mem_vmo, NULL, 0, HEAP_SIZE, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    heap_capacity = HEAP_SIZE;

    malloc_header_t *first = (malloc_header_t *)_mem_addr;
    first->s = HEAP_SIZE - sizeof(malloc_header_t);
    first->is_free = true;
    first->next = NULL;
    first->prev = NULL;
    
    _malloc_free_list = first;
}