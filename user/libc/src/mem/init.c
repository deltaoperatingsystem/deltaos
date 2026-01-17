#include <system.h>
#include <mem.h>

handle_t _mem_vmo = INVALID_HANDLE;
void *_mem_addr = 0;

void _mem_init(void) {
    // initialise the heap
    _mem_vmo = vmo_create(HEAP_SIZE + sizeof(heap_blk_t), 0, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    _mem_addr = vmo_map(_mem_vmo, NULL, 0, HEAP_SIZE + sizeof(heap_blk_t), 3);

    extern void *free;
    free = _mem_addr + sizeof(heap_blk_t);

    //heap starts with only ONE block!!
    *(heap_blk_t*)_mem_addr = (heap_blk_t){ .size = HEAP_SIZE };
}