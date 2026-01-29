#include <obj/handle.h>
#include <obj/namespace.h>
#include <proc/process.h>
#include <fs/fs.h>
#include <obj/kobject.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/path.h>


static process_t *get_handle_owner(void) {
    process_t *proc = process_current();
    if (!proc) {
        proc = process_get_kernel();
    }
    return proc;
}

void handle_init(void) {
    ns_init();
}

static int resolve_full_path(const char *path, char *out, size out_max) {
    if (!path) return -1;
    process_t *proc = process_current();
    if (!proc) proc = process_get_kernel();

    if (path[0] != '/' && path[0] != '$') {
        size cwd_len = strlen(proc->cwd);
        size path_len = strlen(path);
        if (cwd_len + 1 + path_len >= out_max) return -1;
        
        memcpy(out, proc->cwd, cwd_len);
        if (proc->cwd[cwd_len - 1] != '/') {
            out[cwd_len] = '/';
            cwd_len++;
        }
        memcpy(out + cwd_len, path, path_len + 1);
    } else {
        if (strlen(path) >= out_max) return -1;
        strcpy(out, path);
    }
    return 0;
}

handle_t handle_open(const char *path, handle_rights_t rights) {
    if (!path) return INVALID_HANDLE;
    
    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return INVALID_HANDLE;
    
    process_t *proc = get_handle_owner();
    if (!proc) return INVALID_HANDLE;
    
    const char *resolved_path = full_path;
    
    //namespace resolution
    const char *final_path = resolved_path;
    const char *slash = final_path;
    char prefix[64];
    
    if (final_path[0] == '/') {
        //absolute path - default to $files namespace
        //we skip the leading slash for the filesystem-internal path
        strcpy(prefix, "$files");
        slash = final_path;
    } else if (final_path[0] == '$') {
        //namespaced path - extract prefix
        const char *s = final_path + 1;
        while (*s && *s != '/') s++;
        
        size prefix_len = s - final_path;
        if (prefix_len >= sizeof(prefix)) return INVALID_HANDLE;
        memcpy(prefix, final_path, prefix_len);
        prefix[prefix_len] = '\0';
        slash = s;
    } else {
        //should not happen after path resolutiobn
        return INVALID_HANDLE;
    }
    
    object_t *root = ns_lookup(prefix);
    if (!root) {
        //fallback to looking up the entire resolved path
        root = ns_lookup(resolved_path);
        if (!root) return INVALID_HANDLE;
    }
    
    //if it has a lookup operation then use it to find the child
    if (root->ops && root->ops->lookup) {
        //skip the slash if present
        const char *child_path = (*slash == '/') ? (slash + 1) : slash;
        
        //if path is empty or "." it refers to the root/prefix itself
        if (*child_path == '\0' || (child_path[0] == '.' && child_path[1] == '\0')) {
            handle_t h = process_grant_handle(proc, root, rights);
            object_deref(root);
            return h;
        }

        //if it IS an OBJECT_DIR with fs_t data it's a mount point
        //and we might want to use the fs_t specifically but better to just call lookup
        object_t *file = root->ops->lookup(root, child_path);
        object_deref(root);
        if (!file) return INVALID_HANDLE;
        
        handle_t h = process_grant_handle(proc, file, rights);
        object_deref(file);
        return h;
    } else {
        handle_t h = process_grant_handle(proc, root, rights);
        object_deref(root);
        return h;
    }
    
    object_deref(root);
    return INVALID_HANDLE;
}

int handle_create(const char *path, uint32 type) {
    if (!path) return -1;
    
    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return -1;
    
    const char *resolved_path = full_path;
    
    //find first slash
    const char *slash = resolved_path;
    while (*slash && *slash != '/') slash++;
    
    if (*slash != '/') return -1;  //need fs prefix
    
    size prefix_len = slash - resolved_path;
    char prefix[64];
    if (prefix_len >= sizeof(prefix)) return -1;
    
    memcpy(prefix, resolved_path, prefix_len);
    prefix[prefix_len] = '\0';
    
    object_t *root = ns_lookup(prefix);
    if (!root) return -1;
    
    if (root->type == OBJECT_DIR && root->data) {
        fs_t *fs = (fs_t *)root->data;
        if (fs->ops && fs->ops->create) {
            int result = fs->ops->create(fs, slash + 1, type);
            object_deref(root);
            return result;
        }
    }
    object_deref(root);
    return -1;
}

handle_t handle_alloc(object_t *obj, handle_rights_t rights) {
    if (!obj) return INVALID_HANDLE;
    
    process_t *proc = get_handle_owner();
    if (!proc) return INVALID_HANDLE;
    
    return process_grant_handle(proc, obj, rights);
}

object_t *handle_get(handle_t h) {
    process_t *proc = get_handle_owner();
    if (!proc) return NULL;
    return process_get_handle(proc, h);
}

int handle_has_rights(handle_t h, handle_rights_t required) {
    process_t *proc = get_handle_owner();
    if (!proc) return 0;
    return process_handle_has_rights(proc, h, required);
}

handle_t handle_duplicate(handle_t h, handle_rights_t new_rights) {
    process_t *proc = get_handle_owner();
    if (!proc) return INVALID_HANDLE;
    return process_duplicate_handle(proc, h, new_rights);
}

ssize handle_read(handle_t h, void *buf, size len) {
    process_t *proc = get_handle_owner();
    if (!proc) return INVALID_HANDLE;
    
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) return -2;
    if (!entry->obj->ops || !entry->obj->ops->read) return -3;
    
    ssize result = entry->obj->ops->read(entry->obj, buf, len, entry->offset);
    if (result > 0) {
        entry->offset += result;
    }
    return result;
}

ssize handle_write(handle_t h, const void *buf, size len) {
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) return -1;
    if (!entry->obj->ops || !entry->obj->ops->write) return -1;
    
    ssize result = entry->obj->ops->write(entry->obj, buf, len, entry->offset);
    if (result > 0) {
        entry->offset += result;
    }
    return result;
}

ssize handle_seek(handle_t h, ssize offset, int whence) {
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) return -1;
    
    switch (whence) {
        case SEEK_SET:
            entry->offset = offset;
            break;
        case SEEK_CUR:
            entry->offset += offset;
            break;
        case SEEK_END: {
            if (!entry->obj->ops || !entry->obj->ops->stat) return -1;
            stat_t st;
            if (entry->obj->ops->stat(entry->obj, &st) < 0) return -1;
            entry->offset = st.size + offset;
            break;
        }
        default:
            return -1;
    }
    
    return entry->offset;
}

int handle_close(handle_t h) {
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    return process_close_handle(proc, h);
}

int handle_readdir(handle_t h, void *entries, uint32 count) {
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) return -1;
    if (!entry->obj->ops || !entry->obj->ops->readdir) return -1;
    
    uint32 index = (uint32)entry->offset;
    int result = entry->obj->ops->readdir(entry->obj, entries, count, &index);
    if (result >= 0) {
        entry->offset = index;  //update position for next call
    }
    return result;
}

int handle_stat(const char *path, stat_t *st) {
    if (!path || !st) return -1;
    
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    char full_path[512];
    
    //resolve relative paths
    if (path[0] != '/' && path[0] != '$') {
        size cwd_len = strlen(proc->cwd);
        size path_len = strlen(path);
        if (cwd_len + 1 + path_len >= sizeof(full_path)) return -1;
        
        memcpy(full_path, proc->cwd, cwd_len);
        if (proc->cwd[cwd_len - 1] != '/') {
            full_path[cwd_len] = '/';
            cwd_len++;
        }
        memcpy(full_path + cwd_len, path, path_len + 1);
    } else {
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }
    
    //normalize (so handle . and ..)
    path_normalize(full_path);
    
    //namespace resolution
    const char *final_path = full_path;
    const char *slash = final_path;
    char prefix[64];
    
    if (final_path[0] == '/') {
        strcpy(prefix, "$files");
        slash = final_path;
    } else if (final_path[0] == '$') {
        const char *s = final_path + 1;
        while (*s && *s != '/') s++;
        size prefix_len = s - final_path;
        if (prefix_len >= sizeof(prefix)) return -1;
        memcpy(prefix, final_path, prefix_len);
        prefix[prefix_len] = '\0';
        slash = s;
    } else {
        return -1;  //should not happen after normalization
    }
    
    object_t *root = ns_lookup(prefix);
    if (!root) return -1;
    
    int result = -1;
    
    const char *child_path = (*slash == '/') ? (slash + 1) : slash;
    
    if (*child_path == '\0' || (child_path[0] == '.' && child_path[1] == '\0')) {
        if (root->ops && root->ops->stat) {
            result = root->ops->stat(root, st);
        }
    } else if (root->ops && root->ops->lookup) {
        object_t *child = root->ops->lookup(root, child_path);
        if (child) {
            if (child->ops && child->ops->stat) {
                result = child->ops->stat(child, st);
            }
            object_deref(child);
        }
    } else if (root->type == OBJECT_DIR && root->data) {
        fs_t *fs = (fs_t *)root->data;
        if (fs->ops && fs->ops->stat) {
            result = fs->ops->stat(fs, child_path, st);
        }
    }
    
    object_deref(root);
    return result;
}

int handle_fstat(handle_t h, stat_t *st) {
    if (!st) return -1;
    
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    object_t *obj = process_get_handle(proc, h);
    if (!obj) return -1;
    
    int result = -1;
    if (obj->ops && obj->ops->stat) {
        result = obj->ops->stat(obj, st);
    }
    
    return result;
}

int handle_remove(const char *path) {
    if (!path) return -1;
    
    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return -1;
    
    const char *resolved_path = full_path;
    const char *slash = resolved_path;
    char prefix[64];
    
    if (resolved_path[0] == '/') {
        strcpy(prefix, "$files");
        slash = resolved_path;
    } else if (resolved_path[0] == '$') {
        const char *s = resolved_path + 1;
        while (*s && *s != '/') s++;
        size prefix_len = s - resolved_path;
        if (prefix_len >= sizeof(prefix)) return -1;
        memcpy(prefix, resolved_path, prefix_len);
        prefix[prefix_len] = '\0';
        slash = s;
    } else {
        return -1;
    }
    
    object_t *root = ns_lookup(prefix);
    if (!root) return -1;
    
    int result = -1;
    if (root->type == OBJECT_DIR && root->data) {
        fs_t *fs = (fs_t *)root->data;
        if (fs->ops && fs->ops->remove) {
            const char *fs_path = (*slash == '/') ? (slash + 1) : slash;
            result = fs->ops->remove(fs, fs_path);
        }
    }
    
    object_deref(root);
    return result;
}

