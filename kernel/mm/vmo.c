#include <mm/vmo.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <arch/mmu.h>
#include <proc/process.h>
#include <lib/string.h>
#include <lib/io.h>

//VMO object ops
static ssize vmo_obj_read(object_t *obj, void *buf, size len, size offset) {
    vmo_t *vmo = (vmo_t *)obj;
    if (!vmo || !vmo->pages) return -1;
    
    if (offset >= vmo->size) return 0;
    if (offset + len > vmo->size) len = vmo->size - offset;
    
    memcpy(buf, (char *)vmo->pages + offset, len);
    return len;
}

static ssize vmo_obj_write(object_t *obj, const void *buf, size len, size offset) {
    vmo_t *vmo = (vmo_t *)obj;
    if (!vmo || !vmo->pages) return -1;
    
    if (offset >= vmo->size) return 0;
    if (offset + len > vmo->size) len = vmo->size - offset;
    
    memcpy((char *)vmo->pages + offset, buf, len);
    return len;
}

static int vmo_obj_close(object_t *obj) {
    vmo_t *vmo = (vmo_t *)obj;
    if (!vmo) return -1;
    
    //free the backing memory
    if (vmo->pages) {
        size pages = (vmo->size + PAGE_SIZE - 1) / PAGE_SIZE;
        kheap_free_pages(vmo->pages, pages);
        vmo->pages = NULL;
    }
    
    return 0;
}

static object_ops_t vmo_ops = {
    .read = vmo_obj_read,
    .write = vmo_obj_write,
    .close = vmo_obj_close,
    .readdir = NULL,
    .lookup = NULL
};

int32 vmo_create(process_t *proc, size vmo_size, uint32 flags, handle_rights_t rights) {
    if (!proc || vmo_size == 0) return -1;
    
    //allocate VMO structure
    vmo_t *vmo = kzalloc(sizeof(vmo_t));
    if (!vmo) return -1;
    
    //allocate backing memory (for now fully committed)
    size pages = (vmo_size + PAGE_SIZE - 1) / PAGE_SIZE;
    vmo->pages = kheap_alloc_pages(pages);
    if (!vmo->pages) {
        kfree(vmo);
        return -1;
    }
    //raw allocator doesn't zero memory so we must do it manually via HHDM
    memset(vmo->pages, 0, pages * PAGE_SIZE);
    
    //initialize embedded object
    vmo->obj.type = OBJECT_VMO;
    vmo->obj.refcount = 0;
    vmo->obj.ops = &vmo_ops;
    vmo->obj.data = vmo;
    
    vmo->size = vmo_size;
    vmo->committed = vmo_size;
    vmo->flags = flags;
    
    //grant handle to process
    int32 h = process_grant_handle(proc, &vmo->obj, rights);
    if (h < 0) {
        kheap_free_pages(vmo->pages, pages);
        kfree(vmo);
        return -1;
    }
    
    return h;
}

vmo_t *vmo_get(process_t *proc, int32 handle) {
    if (!proc) return NULL;
    
    object_t *obj = process_get_handle(proc, handle);
    if (!obj || obj->type != OBJECT_VMO) return NULL;
    
    return (vmo_t *)obj;
}

ssize vmo_read(process_t *proc, int32 handle, void *buf, size len, size offset) {
    if (!proc || !buf) return -1;
    
    //check read rights
    if (!process_handle_has_rights(proc, handle, HANDLE_RIGHT_READ)) {
        return -2;  //no read permission
    }
    
    vmo_t *vmo = vmo_get(proc, handle);
    if (!vmo) return -1;
    
    return vmo_obj_read(&vmo->obj, buf, len, offset);
}

ssize vmo_write(process_t *proc, int32 handle, const void *buf, size len, size offset) {
    if (!proc || !buf) return -1;
    
    //check write rights
    if (!process_handle_has_rights(proc, handle, HANDLE_RIGHT_WRITE)) {
        return -2;  //no write permission
    }
    
    vmo_t *vmo = vmo_get(proc, handle);
    if (!vmo) return -1;
    
    return vmo_obj_write(&vmo->obj, buf, len, offset);
}

size vmo_get_size(process_t *proc, int32 handle) {
    vmo_t *vmo = vmo_get(proc, handle);
    if (!vmo) return 0;
    return vmo->size;
}

void *vmo_map(process_t *proc, int32 handle, void *vaddr_hint,
              size offset, size len, handle_rights_t map_rights) {
    if (!proc) return NULL;
    
    //check map rights
    proc_handle_t *entry = process_get_handle_entry(proc, handle);
    if (!entry || !(entry->rights & HANDLE_RIGHT_MAP)) {
        return NULL;
    }
    
    //ensure requested mapping rights are allowed by handle
    if ((map_rights & entry->rights) != map_rights) {
        return NULL;
    }
    
    vmo_t *vmo = (vmo_t *)entry->obj;
    if (!vmo) return NULL;
    
    //validate offset and length
    if (offset >= vmo->size) return NULL;
    if (len == 0) len = vmo->size - offset;
    if (offset + len > vmo->size) return NULL;
    
    //for kernel process (NULL pagemap) return direct pointer
    if (!proc->pagemap) {
        return (char *)vmo->pages + offset;
    }
    
    //for user processes we need to map pages into their address space    
    uint64 flags = MMU_FLAG_PRESENT | MMU_FLAG_USER;
    if (map_rights & HANDLE_RIGHT_WRITE) flags |= MMU_FLAG_WRITE;
    if (map_rights & HANDLE_RIGHT_EXECUTE) flags |= MMU_FLAG_EXEC;
    
    //get physical address of VMO pages - must use page table lookup because
    //kernel heap addresses (0xFFFF9...) are not in HHDM range (0xFFFF8...)
    pagemap_t *kmap = mmu_get_kernel_pagemap();
    uint64 phys = mmu_virt_to_phys(kmap, (uintptr)vmo->pages + offset);
    
    //choose virtual address - use hint if provided or allocate from VMA
    uintptr vaddr;
    if (vaddr_hint) {
        vaddr = (uintptr)vaddr_hint;
        //add VMA entry for tracking at specified address
        if (process_vma_add(proc, vaddr, len, flags, &vmo->obj, offset) < 0) {
            return NULL;
        }
    } else {
        //allocate a free region using VMA
        vaddr = process_vma_alloc(proc, len, flags, &vmo->obj, offset);
        if (!vaddr) return NULL;
    }
    
    //map pages
    size pages = (len + 0xFFF) / 0x1000;
    mmu_map_range(proc->pagemap, vaddr, phys, pages, flags);
    
    return (void *)vaddr;
}

int vmo_unmap(process_t *proc, void *vaddr, size len) {
    if (!proc || !vaddr || !proc->pagemap) return -1;
    
    //unmap pages
    size pages = (len + 0xFFF) / 0x1000;
    mmu_unmap_range(proc->pagemap, (uintptr)vaddr, pages);
    
    //remove VMA entry
    process_vma_remove(proc, (uintptr)vaddr);
    
    return 0;
}

typedef struct {
    vmo_t *vmo;
    uintptr new_phys;
    size old_vmo_size;
    size new_vmo_size;
} vmo_update_data_t;

static void vmo_update_mapping_cb(process_t *proc, void *data) {
    vmo_update_data_t *ud = (vmo_update_data_t *)data;
    if (!proc->pagemap) return;

    for (proc_vma_t *vma = proc->vma_list; vma; vma = vma->next) {
        if (vma->obj == &ud->vmo->obj) {
            size old_map_pages = (vma->length + PAGE_SIZE - 1) / PAGE_SIZE;
            
            //unmap old physical pages
            mmu_unmap_range(proc->pagemap, vma->start, old_map_pages);

            //if the VMO grew and this VMA was mapping up to its end, try to grow the VMA
            if (ud->new_vmo_size > ud->old_vmo_size && vma->obj_offset + vma->length == ud->old_vmo_size) {
                size growth = ud->new_vmo_size - ud->old_vmo_size;
                uintptr new_end = vma->start + vma->length + growth;
                
                //check for collisions with other VMAs in this process
                int collision = 0;
                for (proc_vma_t *vma_check = proc->vma_list; vma_check; vma_check = vma_check->next) {
                    if (vma_check == vma) continue;
                    //standard overlap check: [A, B] overlaps with [C, D] if A < D and C < B
                    //A = vma->start + vma->length, B = new_end
                    //C = vma_check->start, D = vma_check->start + vma_check->length
                    if ((vma->start + vma->length) < (vma_check->start + vma_check->length) && vma_check->start < new_end) {
                        collision = 1;
                        break;
                    }
                }

                if (!collision) {
                    vma->length += growth;
                }
            }

            //remap based on new VMO boundaries
            if (vma->obj_offset < ud->new_vmo_size) {
                size remaining_vmo = ud->new_vmo_size - vma->obj_offset;
                size map_len = (vma->length < remaining_vmo) ? vma->length : remaining_vmo;
                size map_pages = (map_len + PAGE_SIZE - 1) / PAGE_SIZE;

                if (map_pages > 0) {
                    uintptr vma_phys = ud->new_phys + vma->obj_offset;
                    mmu_map_range(proc->pagemap, vma->start, vma_phys, map_pages, vma->flags);
                }
            }
        }
    }
}

int vmo_resize(process_t *proc, int32 handle, size new_size) {
    if (!proc || new_size == 0) return -1;
    
    //check write rights for resizing
    if (!process_handle_has_rights(proc, handle, HANDLE_RIGHT_WRITE)) {
        return -3;
    }
    
    vmo_t *vmo = vmo_get(proc, handle);
    if (!vmo) return -1;
    
    if (!(vmo->flags & VMO_FLAG_RESIZABLE)) return -2;
    size old_vmo_size = vmo->size;
    if (new_size == old_vmo_size) return 0;
    
    size old_pages = (old_vmo_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    if (new_pages == old_pages) {
        vmo->size = new_size;
        return 0;
    }
    
    void *new_p = kheap_alloc_pages(new_pages);
    if (!new_p) return -1;
    
    size copy_size = (old_vmo_size < new_size) ? old_vmo_size : new_size;
    memcpy(new_p, vmo->pages, copy_size);

    if (new_pages * PAGE_SIZE > copy_size) {
        memset((char *)new_p + copy_size, 0, (new_pages * PAGE_SIZE) - copy_size);
    }
    
    pagemap_t *kmap = mmu_get_kernel_pagemap();
    uintptr new_phys = mmu_virt_to_phys(kmap, (uintptr)new_p);

    void *old_pages_addr = vmo->pages;
    vmo->pages = new_p;
    vmo->size = new_size;
    vmo->committed = new_size;

    vmo_update_data_t ud = { 
        .vmo = vmo, 
        .new_phys = new_phys, 
        .old_vmo_size = old_vmo_size,
        .new_vmo_size = new_size
    };
    process_iterate(vmo_update_mapping_cb, &ud);

    kheap_free_pages(old_pages_addr, old_pages);
    
    return 0;
}
