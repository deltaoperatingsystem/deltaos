#include <mm/kheap.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <lib/io.h>
#include <lib/string.h>

#define BUCKET_COUNT 8
static slab_cache_t buckets[BUCKET_COUNT];
static size bucket_sizes[BUCKET_COUNT] = {16, 32, 64, 128, 256, 512, 1024, 2048};

static uintptr heap_virt_cursor = KHEAP_VIRT_START;
static bool kheap_ready = false;

//when pages are freed we record the virtual address range as a hole so it
//can be reused by future allocations this prevents unbounded virtual address space
#define VHOLE_MAX_COUNT 256

typedef struct {
    uintptr addr;
    size pages;
    bool in_use;
} vhole_t;

static vhole_t vholes[VHOLE_MAX_COUNT];

//find and reclaim a virtual hole of the exact size needed
static void *backing_alloc(size pages) {
    //try to find an exact-fit hole
    for (int i = 0; i < VHOLE_MAX_COUNT; i++) {
        if (vholes[i].in_use && vholes[i].pages == pages) {
            uintptr vaddr = vholes[i].addr;
            vholes[i].in_use = false;

            void *paddr = pmm_alloc(pages);
            if (!paddr) return NULL;

            vmm_kernel_map(vaddr, (uintptr)paddr, pages, MMU_FLAG_PRESENT | MMU_FLAG_WRITE);
            return (void *)vaddr;
        }
    }

    //no suitable hole found so bump allocate from cursor
    if (heap_virt_cursor + (pages * PAGE_SIZE) > KHEAP_VIRT_END) {
        printf("[kheap] ERR: virtual address space exhausted\n");
        return NULL;
    }

    void *paddr = pmm_alloc(pages);
    if (!paddr) return NULL;

    void *vaddr = (void *)heap_virt_cursor;
    vmm_kernel_map(heap_virt_cursor, (uintptr)paddr, pages, MMU_FLAG_PRESENT | MMU_FLAG_WRITE);
    heap_virt_cursor += pages * PAGE_SIZE;

    return vaddr;
}

//free backing pages and record the virtual range as a hole for reuse
static void backing_free(void *virt, size pages) {
    pagemap_t *map = mmu_get_kernel_pagemap();

    //free each underlying physical page
    for (size i = 0; i < pages; i++) {
        uintptr vaddr = (uintptr)virt + (i * PAGE_SIZE);
        uintptr paddr = mmu_virt_to_phys(map, vaddr);
        if (paddr) {
            pmm_free((void *)paddr, 1);
        }
    }

    //unmap the virtual range
    vmm_unmap(map, (uintptr)virt, pages);

    //record as a hole for future reuse
    for (int i = 0; i < VHOLE_MAX_COUNT; i++) {
        if (!vholes[i].in_use) {
            vholes[i].addr = (uintptr)virt;
            vholes[i].pages = pages;
            vholes[i].in_use = true;
            return;
        }
    }

    //no free slots so virtual address is leaked (rare asf edge case)
    printf("[kheap] WARN: vhole table full, leaking %zu pages at %p\n", pages, virt);
}

static void list_remove(slab_t **head, slab_t *slab) {
    if (slab->prev) slab->prev->next = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    if (*head == slab) *head = slab->next;
    slab->next = slab->prev = NULL;
}

static void list_prepend(slab_t **head, slab_t *slab) {
    slab->next = *head;
    slab->prev = NULL;
    if (*head) (*head)->prev = slab;
    *head = slab;
}


//reate a new slab for the given cache
//the slab header is placed at the start of the page followed by aligned objects
static slab_t *slab_create(slab_cache_t *cache) {
    void *page = backing_alloc(1);
    if (!page) return NULL;

    slab_t *slab = (slab_t *)page;
    slab->magic = KHEAP_MAGIC_SLAB;
    slab->cache = cache;
    slab->next = slab->prev = NULL;

    //calculate aligned start address for objects
    uintptr obj_start = (uintptr)page + sizeof(slab_t);
    size align = cache->obj_size < KHEAP_MIN_ALIGN ? KHEAP_MIN_ALIGN : cache->obj_size;
    obj_start = (obj_start + align - 1) & ~(align - 1);

    //initialize free list
    slab->free_list = (slab_obj_t *)obj_start;
    slab->total_objs = (PAGE_SIZE - (obj_start - (uintptr)page)) / cache->obj_size;
    slab->free_objs = slab->total_objs;

    //chain all objects into the free list
    slab_obj_t *curr = slab->free_list;
    for (uint32 i = 0; i < slab->total_objs - 1; i++) {
        curr->next = (slab_obj_t *)((uintptr)curr + cache->obj_size);
        curr = curr->next;
    }
    curr->next = NULL;

    return slab;
}

//destroy an empty slab and return its memory
static void slab_destroy(slab_t *slab) {
    slab_cache_t *cache = slab->cache;
    list_remove(&cache->empty_slabs, slab);
    backing_free(slab, 1);
}

void kheap_init(void) {
    for (int i = 0; i < BUCKET_COUNT; i++) {
        buckets[i].obj_size = bucket_sizes[i];
        buckets[i].partial_slabs = NULL;
        buckets[i].full_slabs = NULL;
        buckets[i].empty_slabs = NULL;
    }
    kheap_ready = true;
    printf("[kheap] initialized (buckets: 16B-2KB, range: 0x%lx...)\n", KHEAP_VIRT_START);
}

void *kmalloc(size n) {
    if (n == 0 || !kheap_ready) return NULL;

    //try to satisfy from a slab bucket
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (n <= bucket_sizes[i]) {
            slab_cache_t *cache = &buckets[i];

            //find a slab with free space
            slab_t *slab = cache->partial_slabs;
            if (!slab) {
                slab = cache->empty_slabs;
                if (!slab) {
                    slab = slab_create(cache);
                    if (!slab) return NULL;
                } else {
                    list_remove(&cache->empty_slabs, slab);
                }
                list_prepend(&cache->partial_slabs, slab);
            }

            //allocate from the slab's free list
            slab_obj_t *obj = slab->free_list;
            slab->free_list = obj->next;
            slab->free_objs--;

            //move to full list if slab is now exhausted
            if (slab->free_objs == 0) {
                list_remove(&cache->partial_slabs, slab);
                list_prepend(&cache->full_slabs, slab);
            }

            return (void *)obj;
        }
    }

    //when large allocation allocate pages directly with header
    size total = n + sizeof(kheap_large_t);
    //ensure returned pointer is aligned to KHEAP_MIN_ALIGN
    total = (total + KHEAP_MIN_ALIGN - 1) & ~(KHEAP_MIN_ALIGN - 1);
    size pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    kheap_large_t *large = (kheap_large_t *)backing_alloc(pages);
    if (!large) return NULL;

    large->magic = KHEAP_MAGIC_LARGE;
    large->pages = pages;

    //return aligned pointer after header
    uintptr data = (uintptr)large + sizeof(kheap_large_t);
    data = (data + KHEAP_MIN_ALIGN - 1) & ~(KHEAP_MIN_ALIGN - 1);
    return (void *)data;
}

void *kzalloc(size n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;

    //determine allocation type by checking magic at page start
    uintptr page_addr = (uintptr)p & ~(PAGE_SIZE - 1);
    slab_t *meta = (slab_t *)page_addr;

    if (meta->magic == KHEAP_MAGIC_SLAB) {
        //slab allocation: return object to free list
        slab_t *slab = meta;
        slab_cache_t *cache = slab->cache;

        slab_obj_t *obj = (slab_obj_t *)p;
        obj->next = slab->free_list;
        slab->free_list = obj;
        slab->free_objs++;

        //manage slab list transitions
        if (slab->free_objs == 1) {
            //was full and now partial
            list_remove(&cache->full_slabs, slab);
            list_prepend(&cache->partial_slabs, slab);
        } else if (slab->free_objs == slab->total_objs) {
            //was partial and now empty
            list_remove(&cache->partial_slabs, slab);
            list_prepend(&cache->empty_slabs, slab);

            //eagerly destroy this slab if we have other slabs available
            //keep at least one empty slab per cache to avoid thrashing
            bool have_other_slabs = cache->partial_slabs ||
                                    (cache->empty_slabs && cache->empty_slabs->next);
            if (have_other_slabs) {
                slab_destroy(slab);
            }
        }
    } else {
        //large allocation: find header and free pages
        kheap_large_t *large = (kheap_large_t *)page_addr;
        if (large->magic == KHEAP_MAGIC_LARGE) {
            backing_free(large, large->pages);
        } else {
            printf("[kheap] ERR: kfree invalid pointer %p (magic 0x%x)\n", p, meta->magic);
        }
    }
}

void *krealloc(void *p, size n) {
    if (!p) return kmalloc(n);
    if (n == 0) {
        kfree(p);
        return NULL;
    }

    //determine current allocation size
    uintptr page_addr = (uintptr)p & ~(PAGE_SIZE - 1);
    slab_t *meta = (slab_t *)page_addr;

    size old_size = 0;
    if (meta->magic == KHEAP_MAGIC_SLAB) {
        old_size = meta->cache->obj_size;
        if (n <= old_size) return p; //already fits
    } else {
        kheap_large_t *large = (kheap_large_t *)page_addr;
        if (large->magic == KHEAP_MAGIC_LARGE) {
            old_size = (large->pages * PAGE_SIZE) - sizeof(kheap_large_t);
            if (n <= old_size) return p; //already fits
        } else {
            return NULL; //invalid pointer
        }
    }

    //allocate new block ,copy data, free old
    void *new_p = kmalloc(n);
    if (!new_p) return NULL;

    size copy_size = n < old_size ? n : old_size;
    memcpy(new_p, p, copy_size);
    kfree(p);

    return new_p;
}
