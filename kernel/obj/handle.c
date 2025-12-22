#include <obj/handle.h>
#include <obj/namespace.h>
#include <fs/fs.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>

#define HANDLE_INITIAL_CAP 16
#define HANDLE_GROW_FACTOR 2

static handle_entry_t *handles = NULL;
static uint32 handle_capacity = 0;
static uint32 handle_count = 0;

void handle_init(void) {
    handles = kzalloc(HANDLE_INITIAL_CAP * sizeof(handle_entry_t));
    if (!handles) {
        printf("[handle] ERR: failed to allocate handle table\n");
        return;
    }
    handle_capacity = HANDLE_INITIAL_CAP;
    handle_count = 0;
    ns_init();
}

handle_t handle_open(const char *path, uint32 flags) {
    if (!path) return INVALID_HANDLE;
    
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
                
                handle_t h = handle_alloc(file, flags);
                if (h == INVALID_HANDLE) object_deref(file);
                return h;
            }
        }
        object_deref(root);
        return INVALID_HANDLE;
    }
    
    //no slash - direct namespace lookup
    object_t *obj = ns_lookup(path);
    if (!obj) return INVALID_HANDLE;
    
    handle_t h = handle_alloc(obj, flags);
    if (h == INVALID_HANDLE) {
        object_deref(obj);
    }
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

handle_t handle_alloc(object_t *obj, uint32 flags) {
    if (!obj || !handles) return INVALID_HANDLE;
    
    //find free slot
    for (uint32 i = 0; i < handle_capacity; i++) {
        if (handles[i].obj == NULL) {
            handles[i].obj = obj;
            handles[i].offset = 0;
            handles[i].flags = flags;
            handle_count++;
            return i;
        }
    }
    
    //grow table
    uint32 new_cap = handle_capacity * HANDLE_GROW_FACTOR;
    handle_entry_t *new_handles = krealloc(handles, new_cap * sizeof(handle_entry_t));
    if (!new_handles) return INVALID_HANDLE;
    
    //zero new entries
    for (uint32 i = handle_capacity; i < new_cap; i++) {
        new_handles[i].obj = NULL;
        new_handles[i].offset = 0;
        new_handles[i].flags = 0;
    }
    
    handle_t h = handle_capacity;
    new_handles[h].obj = obj;
    new_handles[h].offset = 0;
    new_handles[h].flags = flags;
    
    handles = new_handles;
    handle_capacity = new_cap;
    handle_count++;
    
    return h;
}

object_t *handle_get(handle_t h) {
    if (h < 0 || (uint32)h >= handle_capacity) return NULL;
    return handles[h].obj;
}

ssize handle_read(handle_t h, void *buf, size len) {
    if (h < 0 || (uint32)h >= handle_capacity) return -1;
    
    handle_entry_t *entry = &handles[h];
    if (!entry->obj) return -1;
    if (!entry->obj->ops || !entry->obj->ops->read) return -1;
    
    ssize result = entry->obj->ops->read(entry->obj, buf, len, entry->offset);
    if (result > 0) {
        entry->offset += result;
    }
    return result;
}

ssize handle_write(handle_t h, const void *buf, size len) {
    if (h < 0 || (uint32)h >= handle_capacity) return -1;
    
    handle_entry_t *entry = &handles[h];
    if (!entry->obj) return -1;
    if (!entry->obj->ops || !entry->obj->ops->write) return -1;
    
    ssize result = entry->obj->ops->write(entry->obj, buf, len, entry->offset);
    if (result > 0) {
        entry->offset += result;
    }
    return result;
}

ssize handle_seek(handle_t h, ssize offset, int whence) {
    if (h < 0 || (uint32)h >= handle_capacity) return -1;
    
    handle_entry_t *entry = &handles[h];
    if (!entry->obj) return -1;
    
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
    if (h < 0 || (uint32)h >= handle_capacity) return -1;
    
    handle_entry_t *entry = &handles[h];
    if (!entry->obj) return -1;
    
    object_deref(entry->obj);
    
    entry->obj = NULL;
    entry->offset = 0;
    entry->flags = 0;
    handle_count--;
    
    return 0;
}

int handle_readdir(handle_t h, void *entries, uint32 count) {
    if (h < 0 || (uint32)h >= handle_capacity) return -1;
    
    handle_entry_t *entry = &handles[h];
    if (!entry->obj) return -1;
    if (!entry->obj->ops || !entry->obj->ops->readdir) return -1;
    
    uint32 index = (uint32)entry->offset;
    int result = entry->obj->ops->readdir(entry->obj, entries, count, &index);
    if (result >= 0) {
        entry->offset = index;  //update position for next call
    }
    return result;
}
