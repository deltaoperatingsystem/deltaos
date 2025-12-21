#ifndef MM_KHEAP_H
#define MM_KHEAP_H

#include <arch/types.h>
#include <arch/mmu.h>

//magic numbers for allocation validation
#define KHEAP_MAGIC_SLAB  0x51AB51AB
#define KHEAP_MAGIC_LARGE 0x1A46E1A4

//minimum alignment for all allocations
#define KHEAP_MIN_ALIGN  16

//free object node within a slab
//embedded in the free space of each unallocated object
typedef struct slab_obj {
    struct slab_obj *next;
} slab_obj_t;

struct slab_cache;

//slab header - stored at the start of each slab page
//contains metadata and the free list for this slab
typedef struct slab {
    uint32 magic;               //KHEAP_MAGIC_SLAB for validation
    uint32 total_objs;          //total objects this slab can hold
    struct slab_cache *cache;   //parent cache this slab belongs to
    struct slab *next;          //next slab in list (partial/full/empty)
    struct slab *prev;          //previous slab in list
    slab_obj_t *free_list;      //head of free object list
    uint32 free_objs;           //current number of free objects
} slab_t;

//slab cache - manages slabs for a specific object size
//each bucket size (16, 32, 64, ..., 2048) has one cache
typedef struct slab_cache {
    size obj_size;              //object size for this cache
    slab_t *partial_slabs;      //slabs with some objects allocated
    slab_t *full_slabs;         //slabs with all objects allocated
    slab_t *empty_slabs;        //slabs with no objects allocated
} slab_cache_t;


//header for large allocations (> 2KB)
//stored at the start of the allocated pages
typedef struct {
    size pages;                 //number of pages (putting first to avoid padding)
    uint32 magic;               //KHEAP_MAGIC_LARGE for validation
} kheap_large_t;

//initialize the kernel heap
void kheap_init(void);

//allocate n bytes
void *kmalloc(size n);

//allocate n bytes zero-initialized
void *kzalloc(size n);

//reallocate to new size
void *krealloc(void *p, size n);

//free allocation
void kfree(void *p);

#endif
