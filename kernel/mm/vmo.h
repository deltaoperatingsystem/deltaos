#ifndef MM_VMO_H
#define MM_VMO_H

#include <arch/types.h>
#include <obj/object.h>
#include <obj/rights.h>

/*
 *virtual memory object
 * 
 *aVMO represents a contiguous region of virtual memory that can be:
 *- read from / written to directly
 *- mapped into a process's address space
 *- shared between processes via handle transfer
*/

//VMO flags
#define VMO_FLAG_NONE       0
#define VMO_FLAG_RESIZABLE  (1 << 0)   //can be resized after creation

//forward declarations
struct process;

//VMO structure
typedef struct vmo {
    object_t obj;           //kernel object (embedded)
    void *pages;            //physical memory backing (kernel virtual address)
    size size;              //size in bytes
    size committed;         //actually allocated bytes
    uint32 flags;
} vmo_t;

//create a new VMO of the specified size
//returns handle to the VMO or INVALID_HANDLE
int32 vmo_create(struct process *proc, size size, uint32 flags, handle_rights_t rights);

//get VMO from handle (returns NULL if not a VMO)
vmo_t *vmo_get(struct process *proc, int32 handle);

//read from VMO into buffer
//returns bytes read or negative error
ssize vmo_read(struct process *proc, int32 handle, void *buf, size len, size offset);

//write to VMO from buffer
//returns bytes written or negative error
ssize vmo_write(struct process *proc, int32 handle, const void *buf, size len, size offset);

//get VMO size
size vmo_get_size(struct process *proc, int32 handle);

//map VMO into a process's address space
//returns virtual address or NULL on failure
//vaddr hint can be NULL for kernel to choose address
void *vmo_map(struct process *proc, int32 handle, void *vaddr_hint, 
              size offset, size len, handle_rights_t map_rights);

//unmap VMO from a process's address space
int vmo_unmap(struct process *proc, void *vaddr, size len);

//resize a VMO
//returns 0 on success or negative error
int vmo_resize(struct process *proc, int32 handle, size new_size);

#endif
