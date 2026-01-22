#include <system.h>
#include <mem.h>
#include "internal.h"

handle_t _mem_vmo = INVALID_HANDLE;
void *_mem_addr = 0;
size heap_capacity = 0;

#define HEAP_MAP_HINT (void*)0x4000000000ULL

void _mem_init(void) {
    if (_mem_addr) return;

    //retry until vmo_create succeeds
    while ((_mem_vmo = vmo_create(HEAP_SIZE, VMO_FLAG_RESIZABLE, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)) == INVALID_HANDLE) {
        yield();
    }
    
    //retry until vmo_ma succeeds - use hint to avoid library collisions
    while ((_mem_addr = vmo_map(_mem_vmo, HEAP_MAP_HINT, 0, HEAP_SIZE, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)) == NULL) {
        yield();
    }
    heap_capacity = HEAP_SIZE;

    malloc_header_t *first = (malloc_header_t *)_mem_addr;
    first->s = HEAP_SIZE - sizeof(malloc_header_t);
    first->is_free = true;
    first->next = NULL;
    first->prev = NULL;
    
    _malloc_free_list = first;
}