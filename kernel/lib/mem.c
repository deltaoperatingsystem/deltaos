#include <mm/kheap.h>

void* malloc(size n) {
    return kmalloc(n);
}

void free(void* p) {
    return kfree(p);
}