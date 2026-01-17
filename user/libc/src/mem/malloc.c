#include <mem.h>

// super simple for now!

void *free; // initialised by _mem_init()

#include <io.h>
void *malloc(size size) {
    if (size >= HEAP_SIZE) {    // if attempting to allocate something over HEAP_SIZE, create a personal vmo
        void *block = vmo_map(vmo_create(size + sizeof(heap_blk_t), 0, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP), NULL, 0, size + sizeof(heap_blk_t), 3);
        *(heap_blk_t*)block = (heap_blk_t){ .size = size, .addr = block + sizeof(heap_blk_t) };
        printf("Successfully allocated %d at %X\n", size, block + sizeof(heap_blk_t));
        return block + sizeof(heap_blk_t);
    }
    if ((free + size) > (_mem_addr + HEAP_SIZE + sizeof(heap_blk_t))) return NULL;

    *(heap_blk_t*)free = (heap_blk_t){ .size = size, .addr = free + sizeof(heap_blk_t) };
    printf("successfully allocated %d at %X\n", size, free + sizeof(heap_blk_t));
    free += size + sizeof(heap_blk_t);
    
    return free - size;
}