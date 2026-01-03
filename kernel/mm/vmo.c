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
        kfree(vmo->pages);
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

int32 vmo_create(process_t *proc, size size, uint32 flags, handle_rights_t rights) {
    if (!proc || size == 0) return -1;
    
    //allocate VMO structure
    vmo_t *vmo = kzalloc(sizeof(vmo_t));
    if (!vmo) return -1;
    
    //allocate backing memory (for now fully committed)
    vmo->pages = kzalloc(size);
    if (!vmo->pages) {
        kfree(vmo);
        return -1;
    }
    
    //initialize embedded object
    vmo->obj.type = OBJECT_VMO;
    vmo->obj.refcount = 1;
    vmo->obj.ops = &vmo_ops;
    vmo->obj.data = vmo;
    
    vmo->size = size;
    vmo->committed = size;
    vmo->flags = flags;
    
    //grant handle to process
    int32 h = process_grant_handle(proc, &vmo->obj, rights);
    if (h < 0) {
        kfree(vmo->pages);
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
    if (!process_handle_has_rights(proc, handle, HANDLE_RIGHT_MAP)) {
        return NULL;  //no map permission
    }
    
    vmo_t *vmo = vmo_get(proc, handle);
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
    
    //get physical address of VMO pages using HHDM offset
    uint64 phys = V2P((uintptr)vmo->pages + offset);
    
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
