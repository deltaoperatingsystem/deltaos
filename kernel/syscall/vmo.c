#ifndef SYSCALL_VMO_C
#define SYSCALL_VMO_C

#include <syscall/syscall.h>
#include <mm/vmo.h>
#include <proc/process.h>

intptr sys_vmo_create(size sz, uint32 flags, handle_rights_t rights) {
    if (sz == 0) return -1;
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_create(proc, sz, flags, rights);
}

intptr sys_vmo_read(handle_t h, void *buf, size len, size offset) {
    if (!buf || len == 0) return -1;
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_read(proc, h, buf, len, offset);
}

intptr sys_vmo_write(handle_t h, const void *buf, size len, size offset) {
    if (!buf || len == 0) return -1;
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_write(proc, h, buf, len, offset);
}

intptr sys_vmo_map(handle_t h, uintptr vaddr_hint, size offset, size len, uint32 flags) {
    process_t *proc = process_current();
    if (!proc) return 0;
    
    handle_rights_t map_rights = (handle_rights_t)flags;
    
    void *result = vmo_map(proc, h, (void *)vaddr_hint, offset, len, map_rights);
    return (intptr)(uintptr)result;
}

intptr sys_vmo_unmap(uintptr vaddr, size len) {
    process_t *proc = process_current();
    if (!proc) return -1;
    return vmo_unmap(proc, (void *)vaddr, len);
}

intptr sys_vmo_resize(handle_t vmo_h, size new_size) {
    process_t *current = process_current();
    if (!current) return -1;
    
    return vmo_resize(current, vmo_h, new_size);
}

#endif
