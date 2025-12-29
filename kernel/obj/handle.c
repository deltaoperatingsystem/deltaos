#include <obj/handle.h>
#include <obj/namespace.h>
#include <proc/process.h>
#include <fs/fs.h>
#include <lib/string.h>
#include <lib/io.h>


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

handle_t handle_open(const char *path, handle_rights_t rights) {
    if (!path) return INVALID_HANDLE;
    
    process_t *proc = get_handle_owner();
    if (!proc) return INVALID_HANDLE;
    
    //find first slash to split namespace from subpath
    const char *slash = path;
    while (*slash && *slash != '/') slash++;
    
    if (*slash == '/') {
        //path contains slash so extract namespace prefix
        size prefix_len = slash - path;
        char prefix[64];
        if (prefix_len >= sizeof(prefix)) return INVALID_HANDLE;
        
        memcpy(prefix, path, prefix_len);
        prefix[prefix_len] = '\0';
        
        object_t *root = ns_lookup(prefix);
        if (!root) return INVALID_HANDLE;
        
        //if root is a directory (filesystem) do lookup
        if (root->type == OBJECT_DIR && root->data) {
            fs_t *fs = (fs_t *)root->data;
            if (fs->ops && fs->ops->lookup) {
                object_t *file = fs->ops->lookup(fs, slash + 1);
                object_deref(root);
                if (!file) return INVALID_HANDLE;
                
                handle_t h = process_grant_handle(proc, file, rights);
                object_deref(file);  //grant_handle refs it
                return h;
            }
        }
        object_deref(root);
        return INVALID_HANDLE;
    }
    
    //no slash - direct namespace lookup
    object_t *obj = ns_lookup(path);
    if (!obj) return INVALID_HANDLE;
    
    handle_t h = process_grant_handle(proc, obj, rights);
    object_deref(obj);  //grant_handle refs it
    return h;
}

int handle_create(const char *path, uint32 type) {
    if (!path) return -1;
    
    //find first slash
    const char *slash = path;
    while (*slash && *slash != '/') slash++;
    
    if (*slash != '/') return -1;  //need fs prefix
    
    size prefix_len = slash - path;
    char prefix[64];
    if (prefix_len >= sizeof(prefix)) return -1;
    
    memcpy(prefix, path, prefix_len);
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
    if (!proc) return -1;
    
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (!entry) return -1;
    if (!entry->obj->ops || !entry->obj->ops->read) return -1;
    
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
        case SEEK_END:
            return -1;
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
    
    //find first slash to split namespace from subpath
    const char *slash = path;
    while (*slash && *slash != '/') slash++;
    
    if (*slash != '/') return -1;  //need fs prefix
    
    size prefix_len = slash - path;
    char prefix[64];
    if (prefix_len >= sizeof(prefix)) return -1;
    
    memcpy(prefix, path, prefix_len);
    prefix[prefix_len] = '\0';
    
    object_t *root = ns_lookup(prefix);
    if (!root) return -1;
    
    if (root->type == OBJECT_DIR && root->data) {
        fs_t *fs = (fs_t *)root->data;
        if (fs->ops && fs->ops->stat) {
            int result = fs->ops->stat(fs, slash + 1, st);
            object_deref(root);
            return result;
        }
    }
    object_deref(root);
    return -1;
}

