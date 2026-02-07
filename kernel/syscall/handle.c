#include <syscall/syscall.h>
#include <obj/handle.h>
#include <obj/namespace.h>
#include <proc/process.h>

intptr sys_get_obj(handle_t parent, const char *path, handle_rights_t rights) {
    if (!path) return -1;
    
    if (parent == INVALID_HANDLE) {
        handle_t h = handle_open(path, rights);
        return (intptr)h;
    }
    
    object_t *parent_obj = handle_get(parent);
    if (!parent_obj) return -2;
    
    if (!handle_has_rights(parent, HANDLE_RIGHT_READ)) {
        return -3;
    }
    
    if (!parent_obj->ops || !parent_obj->ops->lookup) {
        return -4;
    }
    
    object_t *child = parent_obj->ops->lookup(parent_obj, path);
    if (!child) {
        return -5;
    }
    
    process_t *proc = process_current();
    if (!proc) {
        object_deref(child);
        return -6;
    }
    
    int h = process_grant_handle(proc, child, rights);
    object_deref(child);
    return (intptr)h;
}

intptr sys_handle_close(handle_t h) {
    process_t *proc = process_current();
    if (!proc) return -1;
    return process_close_handle(proc, h);
}

intptr sys_handle_dup(handle_t h, handle_rights_t new_rights) {
    process_t *proc = process_current();
    if (!proc) return -1;
    return process_duplicate_handle(proc, h, new_rights);
}

intptr sys_ns_register(const char *path, handle_t h) {
    if (!path) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    object_t *obj = process_get_handle(proc, h);
    if (!obj) return -2;
    
    int result = ns_register(path, obj);
    return (intptr)result;
}
