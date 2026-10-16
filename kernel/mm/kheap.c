#include <mm/kheap.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <arch/cpu.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>

#define BUCKET_COUNT 8
static slab_cache_t buckets[BUCKET_COUNT];
static size bucket_sizes[BUCKET_COUNT] = {16, 32, 64, 128, 256, 512, 1024, 2048};

//lock order: kheap_lock (slab state) -> backing_lock (page tracking, holes, kernel page tables)
static spinlock_irq_t kheap_lock = SPINLOCK_IRQ_INIT;
static spinlock_irq_t backing_lock = SPINLOCK_IRQ_INIT;

static uintptr heap_virt_cursor = KHEAP_VIRT_START;  //protected by backing_lock
static bool kheap_ready = false;
static uint64 current_slab_used = 0;      //protected by kheap_lock
static uint64 current_slab_capacity = 0;  //protected by kheap_lock
static uint64 current_large_used = 0;     //updated atomically

//offset of the payload inside a large allocation (header rounded up to the alignment)
#define LARGE_DATA_OFF ((sizeof(kheap_large_t) + KHEAP_MIN_ALIGN - 1) & ~((size)KHEAP_MIN_ALIGN - 1))

typedef enum {
    BACKING_RAW = 1,    //kheap_alloc_pages
    BACKING_SLAB,       //one page owned by a slab cache
    BACKING_LARGE,      //header + payload from kmalloc
} backing_kind_t;

//backing metadata node (either an allocated range or a free virtual hole),
//allocated from HHDM-mapped PMM pages
typedef struct kh_node {
    struct kh_node *hnext;  //track: chain by virt, hole: chain by start, free: pool link
    struct kh_node *enext;  //hole: chain by end
    struct kh_node *bnext;  //hole: size bin list
    struct kh_node *bprev;
    uintptr virt;           //track: range start, hole: range start
    uintptr phys;           //track: physical base
    size pages;
    uint32 kind;            //track only (backing_kind_t)
} kh_node_t;

#define KH_TRACK_BUCKETS 4096
#define KH_HOLE_BUCKETS  1024
#define KH_BIN_COUNT     64   //holes are binned by floor(log2(pages))
#define KH_BIN_SCAN_MAX  32

static kh_node_t *node_pool;
static kh_node_t *track_hash[KH_TRACK_BUCKETS];
static kh_node_t *hole_start_hash[KH_HOLE_BUCKETS];
static kh_node_t *hole_end_hash[KH_HOLE_BUCKETS];
static kh_node_t *hole_bins[KH_BIN_COUNT];

//deferred free queue linked through the freed physical pages via the HHDM
typedef struct kheap_pending {
    struct kheap_pending *next;
    uintptr virt;
    uintptr phys;
    size pages;
} kheap_pending_t;
static kheap_pending_t *pending_head;  //protected by backing_lock

static inline size kh_hash(uintptr addr, size nbuckets) {
    uint64 x = (uint64)(addr >> 12) * 0x9E3779B97F4A7C15ULL;
    return (size)((x >> 32) & (nbuckets - 1));
}

static inline int bin_of(size pages) {
    return 63 - __builtin_clzll(pages);
}

static inline uintptr hole_end(const kh_node_t *h) {
    return h->virt + h->pages * PAGE_SIZE;
}

//caller must hold backing_lock for node/track/hole/vspace helpers

static kh_node_t *node_alloc(void) {
    if (!node_pool) {
        void *phys = pmm_alloc(1);
        if (!phys) return NULL;
        kh_node_t *arr = (kh_node_t *)P2V(phys);
        size count = PAGE_SIZE / sizeof(kh_node_t);
        for (size i = 0; i < count; i++) {
            arr[i].hnext = node_pool;
            node_pool = &arr[i];
        }
    }

    kh_node_t *n = node_pool;
    node_pool = n->hnext;
    memset(n, 0, sizeof(*n));
    return n;
}

static void node_free(kh_node_t *n) {
    n->hnext = node_pool;
    node_pool = n;
}

static kh_node_t *track_find(uintptr virt) {
    for (kh_node_t *n = track_hash[kh_hash(virt, KH_TRACK_BUCKETS)]; n; n = n->hnext) {
        if (n->virt == virt) return n;
    }
    return NULL;
}

static void track_insert(kh_node_t *n) {
    size b = kh_hash(n->virt, KH_TRACK_BUCKETS);
    n->hnext = track_hash[b];
    track_hash[b] = n;
}

static void track_unlink(kh_node_t *n) {
    kh_node_t **pp = &track_hash[kh_hash(n->virt, KH_TRACK_BUCKETS)];
    while (*pp && *pp != n) pp = &(*pp)->hnext;
    if (*pp) *pp = n->hnext;
}

static void hole_link(kh_node_t *h) {
    size s = kh_hash(h->virt, KH_HOLE_BUCKETS);
    h->hnext = hole_start_hash[s];
    hole_start_hash[s] = h;

    size e = kh_hash(hole_end(h), KH_HOLE_BUCKETS);
    h->enext = hole_end_hash[e];
    hole_end_hash[e] = h;

    int b = bin_of(h->pages);
    h->bprev = NULL;
    h->bnext = hole_bins[b];
    if (h->bnext) h->bnext->bprev = h;
    hole_bins[b] = h;
}

static void hole_unlink(kh_node_t *h) {
    kh_node_t **pp = &hole_start_hash[kh_hash(h->virt, KH_HOLE_BUCKETS)];
    while (*pp && *pp != h) pp = &(*pp)->hnext;
    if (*pp) *pp = h->hnext;

    pp = &hole_end_hash[kh_hash(hole_end(h), KH_HOLE_BUCKETS)];
    while (*pp && *pp != h) pp = &(*pp)->enext;
    if (*pp) *pp = h->enext;

    if (h->bprev) h->bprev->bnext = h->bnext;
    else hole_bins[bin_of(h->pages)] = h->bnext;
    if (h->bnext) h->bnext->bprev = h->bprev;
}

static kh_node_t *hole_find_start(uintptr addr) {
    for (kh_node_t *h = hole_start_hash[kh_hash(addr, KH_HOLE_BUCKETS)]; h; h = h->hnext) {
        if (h->virt == addr) return h;
    }
    return NULL;
}

static kh_node_t *hole_find_end(uintptr addr) {
    for (kh_node_t *h = hole_end_hash[kh_hash(addr, KH_HOLE_BUCKETS)]; h; h = h->enext) {
        if (hole_end(h) == addr) return h;
    }
    return NULL;
}

//claim a virtual range of `pages` pages, or 0 if exhausted
static uintptr vspace_take(size pages) {
    //scan the matching size bin, then fall back to the head of any larger bin
    int bin = bin_of(pages);
    kh_node_t *found = NULL;
    int scanned = 0;
    for (kh_node_t *h = hole_bins[bin]; h && scanned < KH_BIN_SCAN_MAX; h = h->bnext, scanned++) {
        if (h->pages >= pages) {
            found = h;
            break;
        }
    }
    for (int b = bin + 1; !found && b < KH_BIN_COUNT; b++) {
        found = hole_bins[b];
    }

    if (found) {
        uintptr vaddr = found->virt;
        hole_unlink(found);
        if (found->pages == pages) {
            node_free(found);
        } else {
            //split the hole
            found->virt += pages * PAGE_SIZE;
            found->pages -= pages;
            hole_link(found);
        }
        return vaddr;
    }

    //no suitable hole found so bump allocate from cursor
    if (pages > (KHEAP_VIRT_END - heap_virt_cursor) / PAGE_SIZE) {
        printf("[kheap] ERR: virtual address space exhausted\n");
        return 0;
    }

    uintptr vaddr = heap_virt_cursor;
    heap_virt_cursor += pages * PAGE_SIZE;
    return vaddr;
}

//return a virtual range to the allocator, merging with neighbouring holes
static void vspace_give(uintptr start, size pages) {
    kh_node_t *reuse = NULL;

    //new range is immediately after an existing hole
    kh_node_t *left = hole_find_end(start);
    if (left) {
        hole_unlink(left);
        start = left->virt;
        pages += left->pages;
        reuse = left;
    }

    //new range is immediately before an existing hole
    kh_node_t *right = hole_find_start(start + pages * PAGE_SIZE);
    if (right) {
        hole_unlink(right);
        pages += right->pages;
        if (reuse) node_free(right);
        else reuse = right;
    }

    //a range touching the cursor just moves the cursor back instead of becoming a hole
    if (start + pages * PAGE_SIZE == heap_virt_cursor) {
        heap_virt_cursor = start;
        if (reuse) node_free(reuse);
        return;
    }

    if (!reuse) {
        reuse = node_alloc();
        if (!reuse) {
            printf("[kheap] WARN: out of metadata memory, leaking %zu virtual pages at %P\n",
                   pages, (void *)start);
            return;
        }
    }
    reuse->virt = start;
    reuse->pages = pages;
    hole_link(reuse);
}

//allocate, map, and track `pages` contiguous pages as `kind`
static void *backing_alloc(size pages, backing_kind_t kind) {
    if (pages == 0 || pages > (KHEAP_VIRT_END - KHEAP_VIRT_START) / PAGE_SIZE) return NULL;

    void *paddr = pmm_alloc(pages);
    if (!paddr) return NULL;

    irq_state_t flags = spinlock_irq_acquire(&backing_lock);

    kh_node_t *node = node_alloc();
    uintptr vaddr = node ? vspace_take(pages) : 0;
    if (!vaddr) {
        if (node) node_free(node);
        spinlock_irq_release(&backing_lock, flags);
        pmm_free(paddr, pages);  //never mapped, so no shootdown needed
        return NULL;
    }

    vmm_kernel_map(vaddr, (uintptr)paddr, pages, MMU_FLAG_PRESENT | MMU_FLAG_WRITE);

    node->virt = vaddr;
    node->phys = (uintptr)paddr;
    node->pages = pages;
    node->kind = kind;
    track_insert(node);

    spinlock_irq_release(&backing_lock, flags);
    return (void *)vaddr;
}

//unmap a tracked range and queue it for deferred shootdown + free (caller holds backing_lock)
static void backing_release_locked(kh_node_t *node) {
    uintptr virt = node->virt;
    uintptr phys = node->phys;
    size pages = node->pages;

    track_unlink(node);
    node_free(node);

    vmm_unmap(mmu_get_kernel_pagemap(), virt, pages);

    kheap_pending_t *pf = (kheap_pending_t *)P2V(phys);
    pf->virt = virt;
    pf->phys = phys;
    pf->pages = pages;
    pf->next = pending_head;
    pending_head = pf;
}

static bool backing_free(void *virt, size pages, backing_kind_t kind) {
    irq_state_t flags = spinlock_irq_acquire(&backing_lock);
    kh_node_t *node = track_find((uintptr)virt);
    if (!node || node->kind != kind || node->pages != pages) {
        spinlock_irq_release(&backing_lock, flags);
        return false;
    }
    backing_release_locked(node);
    spinlock_irq_release(&backing_lock, flags);
    return true;
}

static bool backing_is_block(uintptr head, size pages, backing_kind_t kind) {
    irq_state_t flags = spinlock_irq_acquire(&backing_lock);
    kh_node_t *node = track_find(head);
    bool ok = node && node->kind == kind && node->pages == pages;
    spinlock_irq_release(&backing_lock, flags);
    return ok;
}

static backing_kind_t backing_classify(uintptr head) {
    irq_state_t flags = spinlock_irq_acquire(&backing_lock);
    kh_node_t *node = track_find(head);
    backing_kind_t kind = node ? (backing_kind_t)node->kind : (backing_kind_t)0;
    spinlock_irq_release(&backing_lock, flags);
    return kind;
}

//drain deferred frees outside the locks: shootdown, pmm_free, then recycle virtual range
void kheap_drain(void) {
    //never wait on cross-CPU shootdowns with IRQs disabled
    if (!arch_interrupts_enabled()) {
        return;
    }

    for (;;) {
        irq_state_t flags = spinlock_irq_acquire(&backing_lock);
        kheap_pending_t *pf = pending_head;
        if (!pf) {
            spinlock_irq_release(&backing_lock, flags);
            return;
        }
        pending_head = pf->next;
        //copy out before freeing the backing page that holds pf
        uintptr virt = pf->virt;
        uintptr phys = pf->phys;
        size pages = pf->pages;
        spinlock_irq_release(&backing_lock, flags);

        mmu_tlb_shootdown_range(virt, pages);
        pmm_free((void *)phys, pages);

        flags = spinlock_irq_acquire(&backing_lock);
        vspace_give(virt, pages);
        spinlock_irq_release(&backing_lock, flags);
    }
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

//address of the first object of a slab
static uintptr slab_first_obj(const slab_t *slab, size obj_size) {
    size align = obj_size < KHEAP_MIN_ALIGN ? KHEAP_MIN_ALIGN : obj_size;
    return ((uintptr)slab + sizeof(slab_t) + align - 1) & ~((uintptr)align - 1);
}

//object index for an address, or -1 if it is not exactly the start of an object
static int slab_obj_index(const slab_t *slab, uintptr addr) {
    size obj_size = slab->cache->obj_size;
    uintptr first = slab_first_obj(slab, obj_size);
    if (addr < first) return -1;
    uintptr off = addr - first;
    if ((off % obj_size) != 0) return -1;
    size idx = off / obj_size;
    if (idx >= slab->total_objs) return -1;
    return (int)idx;
}

static inline bool slab_map_test(const slab_t *slab, int idx) {
    return (slab->alloc_map[idx / 64] >> (idx % 64)) & 1;
}

static inline void slab_map_set(slab_t *slab, int idx) {
    slab->alloc_map[idx / 64] |= 1ULL << (idx % 64);
}

static inline void slab_map_clear(slab_t *slab, int idx) {
    slab->alloc_map[idx / 64] &= ~(1ULL << (idx % 64));
}

//reate a new slab for the given cache
//the slab header is placed at the start of the page followed by aligned objects
static slab_t *slab_create(slab_cache_t *cache) {
    void *page = backing_alloc(1, BACKING_SLAB);
    if (!page) return NULL;

    slab_t *slab = (slab_t *)page;
    slab->magic = KHEAP_MAGIC_SLAB;
    slab->cache = cache;
    slab->next = slab->prev = NULL;
    for (size i = 0; i < KHEAP_SLAB_MAX_OBJS / 64; i++) slab->alloc_map[i] = 0;

    //calculate aligned start address for objects
    uintptr obj_start = slab_first_obj(slab, cache->obj_size);

    //initialize free list
    slab->free_list = (slab_obj_t *)obj_start;
    slab->total_objs = (PAGE_SIZE - (obj_start - (uintptr)page)) / cache->obj_size;
    if (slab->total_objs == 0 || slab->total_objs > KHEAP_SLAB_MAX_OBJS) {
        kpanic(NULL, "[kheap] FATAL: slab geometry invalid (obj_size=%zu total=%u)\n",
               cache->obj_size, slab->total_objs);
    }
    slab->free_objs = slab->total_objs;
    current_slab_capacity += (uint64)slab->total_objs * cache->obj_size;

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
    current_slab_capacity -= (uint64)slab->total_objs * cache->obj_size;
    list_remove(&cache->empty_slabs, slab);
    //clear the header so a stale pointer into the recycled page can't look like a slab
    slab->magic = 0;
    backing_free(slab, 1, BACKING_SLAB);
}

void kheap_init(void) {
    for (int i = 0; i < BUCKET_COUNT; i++) {
        buckets[i].obj_size = bucket_sizes[i];
        buckets[i].partial_slabs = NULL;
        buckets[i].full_slabs = NULL;
        buckets[i].empty_slabs = NULL;
    }
    current_slab_used = 0;
    current_slab_capacity = 0;
    __atomic_store_n(&current_large_used, 0, __ATOMIC_RELAXED);
    kheap_ready = true;
    printf("[kheap] initialized (buckets: 16B-2KB, range: 0x%lX...)\n", KHEAP_VIRT_START);
}

void *kmalloc(size n) {
    if (n == 0 || !kheap_ready) return NULL;

    //try to satisfy from a slab bucket
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (n <= bucket_sizes[i]) {
            irq_state_t flags = spinlock_irq_acquire(&kheap_lock);
            slab_cache_t *cache = &buckets[i];

            //find a slab with free space
            slab_t *slab = cache->partial_slabs;
            if (!slab) {
                slab = cache->empty_slabs;
                if (!slab) {
                    slab = slab_create(cache);
                    if (!slab) {
                        spinlock_irq_release(&kheap_lock, flags);
                        return NULL;
                    }
                } else {
                    list_remove(&cache->empty_slabs, slab);
                }
                list_prepend(&cache->partial_slabs, slab);
            }

            //allocate from the slab's free list
            slab_obj_t *obj = slab->free_list;
            //validate free list pointer is within this slab page (including
            //the embedded next pointer) to avoid an OOB read of obj->next
            uintptr slab_start = (uintptr)slab;
            uintptr slab_end = slab_start + PAGE_SIZE;
            int idx = ((uintptr)obj < slab_start || (uintptr)obj + sizeof(slab_obj_t) > slab_end)
                          ? -1 : slab_obj_index(slab, (uintptr)obj);
            //a free object must also be marked free in the bitmap
            if (idx < 0 || slab_map_test(slab, idx)) {
                kpanic(NULL, "[kheap] FATAL: kmalloc free_list %p corrupted "
                       "(slab=%p magic=0x%X cache=%d free_objs=%u total_objs=%u)\n",
                       obj, slab, slab->magic,
                       slab->cache ? (int)((uintptr)slab->cache - (uintptr)buckets) / (int)sizeof(buckets[0]) : -1,
                       slab->free_objs, slab->total_objs);
            }
            slab->free_list = obj->next;
            slab->free_objs--;
            slab_map_set(slab, idx);
            current_slab_used += cache->obj_size;

            //move to full list if slab is now exhausted
            if (slab->free_objs == 0) {
                list_remove(&cache->partial_slabs, slab);
                list_prepend(&cache->full_slabs, slab);
            }

            spinlock_irq_release(&kheap_lock, flags);
            kheap_drain();
            return (void *)obj;
        }
    }

    //when large allocation allocate pages directly with header
    //the returned pointer is aligned after the header, so include that padding in the backing size
    size data_off = LARGE_DATA_OFF;
    if (n > SIZE_MAX - data_off) {
        kheap_drain();
        return NULL;
    }
    size total = data_off + n;
    if (total > SIZE_MAX - (PAGE_SIZE - 1)) {
        kheap_drain();
        return NULL;
    }
    size pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    //only backing_lock is involved (inside backing_alloc), slab users are not blocked
    kheap_large_t *large = (kheap_large_t *)backing_alloc(pages, BACKING_LARGE);
    if (!large) {
        kheap_drain();
        return NULL;
    }

    large->pages = pages;
    large->used_bytes = n;
    large->busy = 0;
    large->magic = KHEAP_MAGIC_LARGE;
    __atomic_fetch_add(&current_large_used, n, __ATOMIC_RELAXED);

    //return aligned pointer after header
    uintptr data = (uintptr)large + data_off;

    kheap_drain();
    return (void *)data;
}

void *kzalloc(size n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

//why a pointer was rejected by the validators below
enum {
    CHK_OK = 0,
    CHK_NOT_BLOCK,    //not the start of a live heap block (freed, interior, raw page, ...)
    CHK_BAD_HEADER,   //header of the block is corrupt
    CHK_BAD_OFFSET,   //inside a block but not at the start of an object
    CHK_DOUBLE_FREE,  //slab object is not currently allocated
    CHK_BUSY,         //large block is being moved by krealloc
};

static const char *chk_reason(int why) {
    switch (why) {
        case CHK_NOT_BLOCK:   return "not a live heap block (already freed, interior pointer or raw page)";
        case CHK_BAD_HEADER:  return "corrupt block header";
        case CHK_BAD_OFFSET:  return "misaligned pointer inside slab";
        case CHK_DOUBLE_FREE: return "double free";
        case CHK_BUSY:        return "block is being reallocated by another caller";
        default:              return "unknown";
    }
}

static void report_bad_ptr(const char *op, void *p, int why) {
    printf("[kheap] ERR: %s of %P rejected: %s\n", op, p, chk_reason(why));
}

//validate that p is a live slab object (caller holds kheap_lock)
static int slab_check_locked(void *p, slab_t **slab_out, int *idx_out) {
    uintptr head = (uintptr)p & ~(PAGE_SIZE - 1);
    if (!backing_is_block(head, 1, BACKING_SLAB)) return CHK_NOT_BLOCK;

    slab_t *slab = (slab_t *)head;
    if (slab->magic != KHEAP_MAGIC_SLAB) return CHK_BAD_HEADER;

    //validate cache ptr is within our known bucket array and aligned
    uintptr cache_addr = (uintptr)slab->cache;
    uintptr buckets_begin = (uintptr)&buckets[0];
    uintptr buckets_end = (uintptr)&buckets[BUCKET_COUNT];
    if (!slab->cache ||
        cache_addr < buckets_begin ||
        cache_addr >= buckets_end ||
        ((cache_addr - buckets_begin) % sizeof(buckets[0])) != 0) {
        return CHK_BAD_HEADER;
    }

    int idx = slab_obj_index(slab, (uintptr)p);
    if (idx < 0) return CHK_BAD_OFFSET;
    if (!slab_map_test(slab, idx)) return CHK_DOUBLE_FREE;

    *slab_out = slab;
    *idx_out = idx;
    return CHK_OK;
}

//return a validated object to its slab (caller holds kheap_lock)
static void slab_free_locked(slab_t *slab, void *p, int idx) {
    slab_cache_t *cache = slab->cache;
    slab_obj_t *obj = (slab_obj_t *)p;

    obj->next = slab->free_list;
    slab->free_list = obj;
    slab->free_objs++;
    slab_map_clear(slab, idx);
    current_slab_used -= cache->obj_size;

    //manage slab list transitions
    if (slab->free_objs == 1) {
        //was full and now partial
        list_remove(&cache->full_slabs, slab);
        list_prepend(&cache->partial_slabs, slab);
    }
    if (slab->free_objs == slab->total_objs) {
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
}

//validate that p is the payload of a live, idle large block (caller holds backing_lock)
static int large_check_locked(void *p, kh_node_t *node) {
    if (!node || node->kind != BACKING_LARGE) return CHK_NOT_BLOCK;
    if ((uintptr)p != node->virt + LARGE_DATA_OFF) return CHK_NOT_BLOCK;

    kheap_large_t *large = (kheap_large_t *)node->virt;
    if (large->magic != KHEAP_MAGIC_LARGE) return CHK_BAD_HEADER;
    if (large->busy) return CHK_BUSY;
    return CHK_OK;
}

static int large_free(void *p, uintptr head) {
    irq_state_t flags = spinlock_irq_acquire(&backing_lock);
    kh_node_t *node = track_find(head);
    int why = large_check_locked(p, node);
    if (why == CHK_OK) {
        kheap_large_t *large = (kheap_large_t *)head;
        __atomic_fetch_sub(&current_large_used, large->used_bytes, __ATOMIC_RELAXED);
        //clear magic before freeing to prevent use-after-free cascades
        large->magic = 0;
        backing_release_locked(node);
    }
    spinlock_irq_release(&backing_lock, flags);
    return why;
}

static int kfree_checked(void *p) {
    if (!p) return KHEAP_OK;

    //range check to ensure this is actually a kernel heap pointer
    uintptr addr = (uintptr)p;
    if (addr < KHEAP_VIRT_START || addr >= KHEAP_VIRT_END) {
        return KHEAP_ERR_INVALID;
    }

    uintptr head = addr & ~(PAGE_SIZE - 1);
    backing_kind_t kind = backing_classify(head);
    int why;

    if (kind == BACKING_SLAB) {
        irq_state_t flags = spinlock_irq_acquire(&kheap_lock);
        slab_t *slab = NULL;
        int idx = -1;
        why = slab_check_locked(p, &slab, &idx);
        if (why == CHK_OK) slab_free_locked(slab, p, idx);
        spinlock_irq_release(&kheap_lock, flags);
    } else if (kind == BACKING_LARGE) {
        why = large_free(p, head);
    } else {
        why = CHK_NOT_BLOCK;
    }

    if (why != CHK_OK) {
        report_bad_ptr("kfree", p, why);
        return KHEAP_ERR_INVALID;
    }

    kheap_drain();
    return KHEAP_OK;
}

void kfree(void *p) {
    (void)kfree_checked(p);
}

static int krealloc_slab(void *p, size n, void **out) {
    irq_state_t flags = spinlock_irq_acquire(&kheap_lock);
    slab_t *slab = NULL;
    int idx = -1;
    int why = slab_check_locked(p, &slab, &idx);
    if (why != CHK_OK) {
        spinlock_irq_release(&kheap_lock, flags);
        report_bad_ptr("krealloc", p, why);
        return KHEAP_ERR_INVALID;
    }
    if (n <= slab->cache->obj_size) {
        spinlock_irq_release(&kheap_lock, flags);
        *out = p; //already fits
        return KHEAP_OK;
    }
    spinlock_irq_release(&kheap_lock, flags);

    void *new_p = kmalloc(n);
    if (!new_p) return KHEAP_ERR_NOMEM;

    //revalidate, copy, and free under kheap_lock so the old object cannot disappear mid-copy
    flags = spinlock_irq_acquire(&kheap_lock);
    why = slab_check_locked(p, &slab, &idx);
    if (why != CHK_OK) {
        spinlock_irq_release(&kheap_lock, flags);
        kfree(new_p);
        report_bad_ptr("krealloc", p, why);
        return KHEAP_ERR_INVALID;
    }
    size old_size = slab->cache->obj_size;
    memcpy(new_p, p, n < old_size ? n : old_size);
    slab_free_locked(slab, p, idx);
    spinlock_irq_release(&kheap_lock, flags);

    kheap_drain();
    *out = new_p;
    return KHEAP_OK;
}

static int krealloc_large(void *p, size n, uintptr head, void **out) {
    irq_state_t flags = spinlock_irq_acquire(&backing_lock);
    kh_node_t *node = track_find(head);
    int why = large_check_locked(p, node);
    if (why != CHK_OK) {
        spinlock_irq_release(&backing_lock, flags);
        report_bad_ptr("krealloc", p, why);
        return KHEAP_ERR_INVALID;
    }

    kheap_large_t *large = (kheap_large_t *)head;
    size old_capacity = node->pages * PAGE_SIZE - LARGE_DATA_OFF;
    size old_used = large->used_bytes;
    if (old_used > old_capacity) old_used = old_capacity;

    if (n <= old_capacity) {
        __atomic_fetch_sub(&current_large_used, large->used_bytes, __ATOMIC_RELAXED);
        __atomic_fetch_add(&current_large_used, n, __ATOMIC_RELAXED);
        large->used_bytes = n;
        spinlock_irq_release(&backing_lock, flags);
        *out = p;
        return KHEAP_OK;
    }

    //pin the block with busy=1 so we can copy without holding backing_lock
    large->busy = 1;
    spinlock_irq_release(&backing_lock, flags);

    void *new_p = kmalloc(n);
    if (!new_p) {
        flags = spinlock_irq_acquire(&backing_lock);
        large->busy = 0;
        spinlock_irq_release(&backing_lock, flags);
        return KHEAP_ERR_NOMEM;
    }

    memcpy(new_p, p, n < old_used ? n : old_used);

    flags = spinlock_irq_acquire(&backing_lock);
    node = track_find(head);
    if (!node || node->kind != BACKING_LARGE) {
        spinlock_irq_release(&backing_lock, flags);
        kfree(new_p);
        report_bad_ptr("krealloc", p, CHK_NOT_BLOCK);
        return KHEAP_ERR_INVALID;
    }
    large->busy = 0;
    __atomic_fetch_sub(&current_large_used, large->used_bytes, __ATOMIC_RELAXED);
    large->magic = 0;
    backing_release_locked(node);
    spinlock_irq_release(&backing_lock, flags);

    kheap_drain();
    *out = new_p;
    return KHEAP_OK;
}

int krealloc_ex(void *p, size n, void **out) {
    void *dummy;
    if (!out) out = &dummy;
    *out = NULL;

    if (!p) {
        void *np = kmalloc(n);
        *out = np;
        return (np || n == 0) ? KHEAP_OK : KHEAP_ERR_NOMEM;
    }
    if (n == 0) {
        return kfree_checked(p);
    }

    uintptr addr = (uintptr)p;
    if (addr < KHEAP_VIRT_START || addr >= KHEAP_VIRT_END) {
        report_bad_ptr("krealloc", p, CHK_NOT_BLOCK);
        return KHEAP_ERR_INVALID;
    }

    uintptr head = addr & ~(PAGE_SIZE - 1);
    switch (backing_classify(head)) {
        case BACKING_SLAB:  return krealloc_slab(p, n, out);
        case BACKING_LARGE: return krealloc_large(p, n, head, out);
        default:
            report_bad_ptr("krealloc", p, CHK_NOT_BLOCK);
            return KHEAP_ERR_INVALID;
    }
}

void *krealloc(void *p, size n) {
    void *out;
    if (krealloc_ex(p, n, &out) != KHEAP_OK) return NULL;
    return out;
}

void *kheap_alloc_pages(size pages) {
    void *p = backing_alloc(pages, BACKING_RAW);
    kheap_drain();
    return p;
}

void kheap_free_pages(void *p, size pages) {
    if (!backing_free(p, pages, BACKING_RAW)) {
        printf("[kheap] ERR: kheap_free_pages of %P (%zu pages) rejected: not a matching raw allocation\n",
               p, pages);
    }
    kheap_drain();
}

void kheap_get_stats(kheap_stats_t *stats) {
    if (!stats) return;
    
    irq_state_t flags = spinlock_irq_acquire(&kheap_lock);
    
    stats->slab_used = current_slab_used;
    stats->slab_capacity = current_slab_capacity;
    
    spinlock_irq_release(&kheap_lock, flags);

    stats->large_used = __atomic_load_n(&current_large_used, __ATOMIC_RELAXED);
}
