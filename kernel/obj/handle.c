#include <obj/handle.h>
#include <obj/namespace.h>
#include <proc/process.h>
#include <fs/fs.h>
#include <obj/kobject.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/path.h>
#include <fs/mount.h>


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

    path_normalize(out);
    return 0;
}

static int resolve_mounted_path(const char *resolved_path, fs_t **fs_out, const char **fs_path_out) {
    if (!resolved_path || resolved_path[0] != '/') return 0;
    return fs_mount_resolve(resolved_path, fs_out, fs_path_out);
}

handle_t handle_open(const char *path, handle_rights_t rights) {
    if (!path) return INVALID_HANDLE;

    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return INVALID_HANDLE;

    process_t *proc = get_handle_owner();
    if (!proc) return INVALID_HANDLE;

    const char *resolved_path = full_path;

    //mounted absolute paths take precedence over the $files namespace fallback
    if (resolved_path[0] == '/') {
        fs_t *fs = NULL;
        const char *fs_path = NULL;
        if (resolve_mounted_path(resolved_path, &fs, &fs_path) && fs && fs->ops && fs->ops->lookup) {
            object_t *obj = fs->ops->lookup(fs, fs_path);
            if (!obj) return INVALID_HANDLE;
            handle_t h = process_grant_handle(proc, obj, rights);
            object_deref(obj);
            return h;
        }

        //unmounted absolute path: rewrite as $files/path for namespace lookup
        char ns_path[512];
        if (snprintf(ns_path, sizeof(ns_path), "$files%s", resolved_path) >= (int)sizeof(ns_path))
            return INVALID_HANDLE;
        resolved_path = ns_path;
    }

    //namespace lookup: the tree handles all traversal including factory delegation
    handle_rights_t ceiling = HANDLE_RIGHTS_ALL;
    object_t *obj = ns_lookup_ex(resolved_path, &ceiling);
    if (!obj) return INVALID_HANDLE;

    handle_t h = process_grant_handle(proc, obj, rights & ceiling);
    object_deref(obj);
    return h;
}

int handle_create(const char *path, uint32 type) {
    if (!path) return -1;
    
    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return -1;
    
    const char *resolved_path = full_path;
    const char *slash = resolved_path;
    char prefix[64];

    //mounted filesystem path
    if (resolved_path[0] == '/') {
        fs_t *fs = NULL;
        const char *fs_path = NULL;
        if (resolve_mounted_path(resolved_path, &fs, &fs_path) && fs && fs->ops && fs->ops->create) {
            return fs->ops->create(fs, fs_path, type);
        }

        //absolute path - default to $files namespace
        //we skip the leading slash for the filesystem-internal path
        strcpy(prefix, "$files");
        slash = resolved_path;
    } else {
        //legacy namespace fallback
        while (*slash && *slash != '/') slash++;
        
        if (*slash != '/') return -1;  //need fs prefix
        
        size prefix_len = slash - resolved_path;
        if (prefix_len >= sizeof(prefix)) return -1;
        
        memcpy(prefix, resolved_path, prefix_len);
        prefix[prefix_len] = '\0';
    }
    
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
    if (!proc) return -1;

    //ref the object before releasing the lock to avoid concurrent close UAF
    spinlock_acquire(&proc->lock);
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) { spinlock_release(&proc->lock); return -2; }
    if (!rights_has(entry->rights, HANDLE_RIGHT_READ)) { spinlock_release(&proc->lock); return -2; }
    if (!entry->obj->ops || !entry->obj->ops->read) { spinlock_release(&proc->lock); return -3; }
    object_t *obj = entry->obj;
    size offset = entry->offset;
    object_ref(obj);
    spinlock_release(&proc->lock);

    ssize result = obj->ops->read(obj, buf, len, offset);
    if (result > 0) {
        //best effort offset update may race with concurrent reads
        spinlock_acquire(&proc->lock);
        proc_handle_t *e2 = process_get_handle_entry(proc, h);
        if (e2 && e2->obj == obj) e2->offset += result;
        spinlock_release(&proc->lock);
    }
    object_deref(obj);
    return result;
}

ssize handle_write(handle_t h, const void *buf, size len) {
    process_t *proc = get_handle_owner();
    if (!proc) return -1;

    //ref the object before releasing the lock to avoid concurrent close UAF
    spinlock_acquire(&proc->lock);
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) { spinlock_release(&proc->lock); return -1; }
    if (!rights_has(entry->rights, HANDLE_RIGHT_WRITE)) { spinlock_release(&proc->lock); return -2; }
    if (!entry->obj->ops || !entry->obj->ops->write) { spinlock_release(&proc->lock); return -1; }
    object_t *obj = entry->obj;
    size offset = entry->offset;
    object_ref(obj);
    spinlock_release(&proc->lock);

    ssize result = obj->ops->write(obj, buf, len, offset);
    if (result > 0) {
        spinlock_acquire(&proc->lock);
        proc_handle_t *e2 = process_get_handle_entry(proc, h);
        if (e2 && e2->obj == obj) e2->offset += result;
        spinlock_release(&proc->lock);
    }
    object_deref(obj);
    return result;
}

ssize handle_seek(handle_t h, ssize offset, int whence) {
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) return -1;
    if (!rights_has(entry->rights, HANDLE_RIGHT_READ) &&
        !rights_has(entry->rights, HANDLE_RIGHT_WRITE)) return -2;
    
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

    //ref the object before releasing the lock to avoid concurrent close UAF
    spinlock_acquire(&proc->lock);
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) { spinlock_release(&proc->lock); return -1; }
    if (!rights_has(entry->rights, HANDLE_RIGHT_READ)) { spinlock_release(&proc->lock); return -2; }
    if (!entry->obj->ops || !entry->obj->ops->readdir) { spinlock_release(&proc->lock); return -1; }
    object_t *obj = entry->obj;
    uint32 index = (uint32)entry->offset;
    object_ref(obj);
    spinlock_release(&proc->lock);

    int result = obj->ops->readdir(obj, entries, count, &index);
    if (result >= 0) {
        spinlock_acquire(&proc->lock);
        proc_handle_t *e2 = process_get_handle_entry(proc, h);
        if (e2 && e2->obj == obj) e2->offset = index;
        spinlock_release(&proc->lock);
    }
    object_deref(obj);
    return result;
}

int handle_stat(const char *path, stat_t *st) {
    if (!path || !st) return -1;
    
    process_t *proc = get_handle_owner();
    if (!proc) return -1;
    
    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return -1;

    //check mounted filesystems first so /mnt/foo resolves to the mounted FS
    if (full_path[0] == '/') {
        fs_t *fs = NULL;
        const char *fs_path = NULL;
        if (resolve_mounted_path(full_path, &fs, &fs_path) && fs && fs->ops && fs->ops->lookup) {
            object_t *obj = fs->ops->lookup(fs, fs_path);
            if (!obj) return -1;
            int result = -1;
            if (obj->ops && obj->ops->stat) {
                result = obj->ops->stat(obj, st);
            }
            object_deref(obj);
            return result;
        }
    }
    
    //for $-prefixed namespace paths we use ns_lookup_ex so multi-level paths like
    //$devices/disks/nvme1n1p1 are walked correctly through the namespace tree
    if (full_path[0] == '$') {
        object_t *obj = ns_lookup_ex(full_path, NULL);
        if (!obj) return -1;
        int result = -1;
        if (obj->ops && obj->ops->stat) {
            result = obj->ops->stat(obj, st);
        }
        object_deref(obj);
        return result;
    }

    //legacy namespace resolution for non-$ paths (absolute paths rewritten as $files/...)
    const char *final_path = full_path;
    const char *slash = final_path;
    char prefix[64];
    
    if (final_path[0] == '/') {
        strcpy(prefix, "$files");
        slash = final_path;
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

    //ref the object before releasing the lock to avoid concurrent close UAF
    spinlock_acquire(&proc->lock);
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) { spinlock_release(&proc->lock); return -1; }
    if (!rights_has(entry->rights, HANDLE_RIGHT_GET_INFO)) { spinlock_release(&proc->lock); return -2; }
    object_t *obj = entry->obj;
    object_ref(obj);
    spinlock_release(&proc->lock);

    int result = -1;
    if (obj->ops && obj->ops->stat) {
        result = obj->ops->stat(obj, st);
    }
    object_deref(obj);
    return result;
}

int handle_remove(const char *path) {
    if (!path) return -1;
    
    char full_path[512];
    if (resolve_full_path(path, full_path, sizeof(full_path)) < 0) return -1;

    //mounted filesystem path
    if (full_path[0] == '/') {
        fs_t *fs = NULL;
        const char *fs_path = NULL;
        if (resolve_mounted_path(full_path, &fs, &fs_path) && fs && fs->ops && fs->ops->remove) {
            return fs->ops->remove(fs, fs_path);
        }
    }
    
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
