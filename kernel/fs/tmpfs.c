#include <fs/tmpfs.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/spinlock.h>
#include <drivers/rtc.h>

#define TMPFS_MAX_NAME 64
#define TMPFS_INITIAL_BUF 256
#define TMPFS_INITIAL_CHILDREN 8

//get current time as seconds since 2000-01-01
static uint32 get_current_time(void) {
    static const uint8 days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    rtc_time_t t;
    rtc_get_time(&t);
    
    uint32 days = 0;
    for (uint32 y = 2000; y < t.year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    for (uint8 m = 1; m < t.month; m++) {
        days += days_in_month[m - 1];
        if (m == 2 && (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0))) days++;
    }
    days += t.day - 1;
    
    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

//tmpfs node (file or directory)
typedef struct tmpfs_node {
    char name[TMPFS_MAX_NAME];
    uint32 type;  //FS_TYPE_FILE or FS_TYPE_DIR
    uint32 ctime; //creation time (seconds since 2000)
    struct tmpfs_node *parent;
    uint32 refcount; //number of live object_t referencing this node
    bool unlinked;   //removed from tree but still open
    
    union {
        //file data
        struct {
            uint8 *data;
            size size;
            size capacity;
        } file;
        //directory data
        struct {
            struct tmpfs_node **children;
            uint32 count;
            uint32 capacity;
        } dir;
    };
} tmpfs_node_t;

//root directory
static tmpfs_node_t *root = NULL;
static spinlock_t tmpfs_lock = {0};

//caller must hold tmpfs_lock
static void tmpfs_node_hold_locked(tmpfs_node_t *node) {
    if (node) node->refcount++;
}

//caller must hold tmpfs_lock; frees node once unlinked and unreferenced
static void tmpfs_node_release_locked(tmpfs_node_t *node) {
    if (!node || node->refcount == 0) return;
    node->refcount--;
    if (node->refcount == 0 && node->unlinked) {
        if (node->type == FS_TYPE_FILE) {
            kfree(node->file.data);
        } else {
            for (uint32 i = 0; i < node->dir.count; i++) {
                tmpfs_node_t *child = node->dir.children[i];
                if (child) {
                    child->parent = NULL;
                    child->unlinked = true;
                    if (child->refcount == 0) {
                        if (child->type == FS_TYPE_FILE) kfree(child->file.data);
                        else kfree(child->dir.children);
                        kfree(child);
                    }
                }
            }
            kfree(node->dir.children);
        }
        kfree(node);
    }
}

static int tmpfs_file_close(object_t *obj);
static int tmpfs_dir_close(object_t *obj);

//find child by name in directory (caller must hold tmpfs_lock; skips unlinked)
static tmpfs_node_t *find_child(tmpfs_node_t *dir, const char *name) {
    if (!dir || dir->type != FS_TYPE_DIR) return NULL;
    
    for (uint32 i = 0; i < dir->dir.count; i++) {
        tmpfs_node_t *c = dir->dir.children[i];
        if (!c || c->unlinked) continue;
        if (strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return NULL;
}

//add child to directory
static int add_child(tmpfs_node_t *dir, tmpfs_node_t *child) {
    if (!dir || dir->type != FS_TYPE_DIR) return -1;
    
    if (dir->dir.count >= dir->dir.capacity) {
        uint32 new_cap = dir->dir.capacity * 2;
        tmpfs_node_t **new_children = krealloc(dir->dir.children, 
                                                new_cap * sizeof(tmpfs_node_t *));
        if (!new_children) return -1;
        dir->dir.children = new_children;
        dir->dir.capacity = new_cap;
    }
    
    dir->dir.children[dir->dir.count++] = child;
    child->parent = dir;
    return 0;
}

//resolve path to node, optionally creating missing directories
static tmpfs_node_t *resolve_path(const char *path, bool create_dirs, tmpfs_node_t **parent_out, char *basename_out) {
    if (!path || !root) return NULL;
    
    tmpfs_node_t *current = root;
    char component[TMPFS_MAX_NAME];
    const char *p = path;
    
    while (*p) {
        //skip leading slashes
        while (*p == '/') p++;
        if (!*p) break;
        
        //extract component
        const char *end = p;
        while (*end && *end != '/') end++;
        
        size len = end - p;
        if (len >= TMPFS_MAX_NAME) return NULL;
        
        memcpy(component, p, len);
        component[len] = '\0';
        
        //check if this is the last component
        const char *next = end;
        while (*next == '/') next++;
        bool is_last = (*next == '\0');
        
        if (is_last) {
            //return parent and basename for create operations
            if (parent_out) *parent_out = current;
            if (basename_out) {
                memcpy(basename_out, component, len + 1);
            }
            return find_child(current, component);
        }
        
        //navigate to next directory
        tmpfs_node_t *next_node = find_child(current, component);
        if (!next_node) {
            if (!create_dirs) return NULL;
            
            //create missing directory
            next_node = kzalloc(sizeof(tmpfs_node_t));
            if (!next_node) return NULL;
            
            strncpy(next_node->name, component, TMPFS_MAX_NAME - 1);
            next_node->name[TMPFS_MAX_NAME - 1] = '\0';
            next_node->type = FS_TYPE_DIR;
            next_node->refcount = 0;
            next_node->unlinked = false;
            next_node->dir.children = kzalloc(TMPFS_INITIAL_CHILDREN * sizeof(tmpfs_node_t *));
            if (!next_node->dir.children) { kfree(next_node); return NULL; }
            next_node->dir.capacity = TMPFS_INITIAL_CHILDREN;
            next_node->dir.count = 0;
            
            if (add_child(current, next_node) < 0) {
                kfree(next_node->dir.children);
                kfree(next_node);
                return NULL;
            }
        }
        
        if (next_node->type != FS_TYPE_DIR) return NULL;
        current = next_node;
        p = end;
    }
    
    return current;
}

//file object read
static ssize tmpfs_file_read(object_t *obj, void *buf, size len, size offset) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_FILE) return -1;
    
    spinlock_acquire(&tmpfs_lock);
    if (offset >= node->file.size) {
        spinlock_release(&tmpfs_lock);
        return 0;
    }
    
    size avail = node->file.size - offset;
    size to_read = len < avail ? len : avail;
    memcpy(buf, node->file.data + offset, to_read);
    spinlock_release(&tmpfs_lock);
    return to_read;
}

//file object write
static ssize tmpfs_file_write(object_t *obj, const void *buf, size len, size offset) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_FILE) return -1;
    
    spinlock_acquire(&tmpfs_lock);
    
    if (len > SIZE_MAX - offset) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    size end = offset + len;
    
    if (end > node->file.capacity) {
        size new_cap = node->file.capacity ? node->file.capacity : TMPFS_INITIAL_BUF;
        while (new_cap < end) {
            if (new_cap > SIZE_MAX / 2) { new_cap = SIZE_MAX; break; }
            new_cap *= 2;
        }
        
        uint8 *new_data = krealloc(node->file.data, new_cap);
        if (!new_data) {
            spinlock_release(&tmpfs_lock);
            return -1;
        }
        
        node->file.data = new_data;
        node->file.capacity = new_cap;
    }
    
    memcpy(node->file.data + offset, buf, len);
    if (end > node->file.size) node->file.size = end;
    
    spinlock_release(&tmpfs_lock);
    return len;
}

static int tmpfs_file_stat(object_t *obj, stat_t *st) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || !st) return -1;
    //read fields under lock to prevent torn reads on SMP
    spinlock_acquire(&tmpfs_lock);
    st->type = FS_TYPE_FILE;
    st->size = node->file.size;
    st->ctime = st->mtime = st->atime = node->ctime;
    spinlock_release(&tmpfs_lock);
    return 0;
}

static int tmpfs_file_close(object_t *obj) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node) return 0;
    obj->data = NULL;
    spinlock_acquire(&tmpfs_lock);
    tmpfs_node_release_locked(node);
    spinlock_release(&tmpfs_lock);
    return 0;
}

static int tmpfs_dir_close(object_t *obj) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node) return 0;
    obj->data = NULL;
    spinlock_acquire(&tmpfs_lock);
    tmpfs_node_release_locked(node);
    spinlock_release(&tmpfs_lock);
    return 0;
}

static object_ops_t tmpfs_file_ops = {
    .read = tmpfs_file_read,
    .write = tmpfs_file_write,
    .close = tmpfs_file_close,
    .readdir = NULL,
    .lookup = NULL,
    .stat = tmpfs_file_stat
};

//directory object readdir
static int tmpfs_dir_readdir(object_t *obj, void *buf, uint32 count, uint32 *index) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_DIR) return -1;
    
    dirent_t *entries = (dirent_t *)buf;
    //hold lock to prevent concurrent create/remove from modifying the children array
    spinlock_acquire(&tmpfs_lock);
    uint32 i = *index;
    uint32 filled = 0;
    
    for (; i < node->dir.count && filled < count; i++) {
        tmpfs_node_t *c = node->dir.children[i];
        if (!c || c->unlinked) continue;
        strncpy(entries[filled].name, c->name, sizeof(entries[filled].name) - 1);
        entries[filled].name[sizeof(entries[filled].name) - 1] = '\0';
        entries[filled].type = c->type;
        filled++;
    }
    
    *index = i;
    spinlock_release(&tmpfs_lock);
    return filled;
}

//forward declaration for recursive reference
static object_ops_t tmpfs_dir_ops;

static int tmpfs_dir_stat(object_t *obj, stat_t *st) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || !st) return -1;
    //read ctime under lock to prevent tearing
    spinlock_acquire(&tmpfs_lock);
    st->type = FS_TYPE_DIR;
    st->size = 0;
    st->ctime = st->mtime = st->atime = node->ctime;
    spinlock_release(&tmpfs_lock);
    return 0;
}

//directory lookup - find child by name
static object_t *tmpfs_dir_lookup(object_t *obj, const char *name) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || !name || node->type != FS_TYPE_DIR) return NULL;
    
    spinlock_acquire(&tmpfs_lock);
    tmpfs_node_t *child = find_child(node, name);
    if (child) tmpfs_node_hold_locked(child);
    uint32 child_type = child ? child->type : 0;
    spinlock_release(&tmpfs_lock);
    if (!child) return NULL;

    object_t *result = NULL;
    if (child_type == FS_TYPE_FILE) {
        result = object_create(OBJECT_FILE, &tmpfs_file_ops, child);
    } else if (child_type == FS_TYPE_DIR) {
        result = object_create(OBJECT_DIR, &tmpfs_dir_ops, child);
    }
    if (!result) {
        spinlock_acquire(&tmpfs_lock);
        tmpfs_node_release_locked(child);
        spinlock_release(&tmpfs_lock);
        return NULL;
    }
    return result;
}

static object_ops_t tmpfs_dir_ops = {
    .read = NULL,
    .write = NULL,
    .close = tmpfs_dir_close,
    .readdir = tmpfs_dir_readdir,
    .lookup = tmpfs_dir_lookup,
    .stat = tmpfs_dir_stat
};

//filesystem ops
static object_t *tmpfs_fs_lookup(fs_t *fs, const char *path) {
    (void)fs;
    spinlock_acquire(&tmpfs_lock);
    tmpfs_node_t *node = resolve_path(path, false, NULL, NULL);
    if (node) tmpfs_node_hold_locked(node);
    uint32 node_type = node ? node->type : 0;
    spinlock_release(&tmpfs_lock);
    if (!node) return NULL;
    
    object_t *obj = NULL;
    if (node_type == FS_TYPE_FILE) {
        obj = object_create(OBJECT_FILE, &tmpfs_file_ops, node);
    } else if (node_type == FS_TYPE_DIR) {
        obj = object_create(OBJECT_DIR, &tmpfs_dir_ops, node);
    }
    if (!obj) {
        spinlock_acquire(&tmpfs_lock);
        tmpfs_node_release_locked(node);
        spinlock_release(&tmpfs_lock);
        return NULL;
    }
    return obj;
}

static int tmpfs_fs_create(fs_t *fs, const char *path, uint32 type) {
    (void)fs;
    
    spinlock_acquire(&tmpfs_lock);
    
    tmpfs_node_t *parent = NULL;
    char basename[TMPFS_MAX_NAME];
    
    //check if already exists
    if (resolve_path(path, false, NULL, NULL)) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    
    //resolve path and create parent dirs
    resolve_path(path, true, &parent, basename);
    if (!parent || parent->type != FS_TYPE_DIR) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    
    //create new node
    tmpfs_node_t *node = kzalloc(sizeof(tmpfs_node_t));
    if (!node) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    
    strncpy(node->name, basename, TMPFS_MAX_NAME - 1);
    node->name[TMPFS_MAX_NAME - 1] = '\0';
    node->type = type;
    node->ctime = get_current_time();
    node->refcount = 0;
    node->unlinked = false;
    
    if (type == FS_TYPE_DIR) {
        node->dir.children = kzalloc(TMPFS_INITIAL_CHILDREN * sizeof(tmpfs_node_t *));
        if (!node->dir.children) {
            kfree(node);
            spinlock_release(&tmpfs_lock);
            return -1;
        }
        node->dir.capacity = TMPFS_INITIAL_CHILDREN;
        node->dir.count = 0;
    } else {
        node->file.data = NULL;
        node->file.size = 0;
        node->file.capacity = 0;
    }
    
    if (add_child(parent, node) != 0) {
        if (type == FS_TYPE_DIR) kfree(node->dir.children);
        kfree(node);
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    spinlock_release(&tmpfs_lock);
    return 0;
}

static int tmpfs_fs_rename(fs_t *fs, const char *old_path, const char *new_path) {
    (void)fs;

    spinlock_acquire(&tmpfs_lock);

    //resolve old path
    tmpfs_node_t *old_parent = NULL;
    char old_basename[TMPFS_MAX_NAME];
    tmpfs_node_t *node = resolve_path(old_path, false, &old_parent, old_basename);
    if (!node || !old_parent) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    if (node == root) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }

    //resolve new path - get parent and basename, ensure no existing node
    tmpfs_node_t *new_parent = NULL;
    char new_basename[TMPFS_MAX_NAME];
    if (resolve_path(new_path, false, &new_parent, new_basename)) {
        //new path already exists (same-path rename is a no-op success)
        if (strcmp(old_path, new_path) == 0) {
            spinlock_release(&tmpfs_lock);
            return 0;
        }
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    if (!new_parent || new_parent->type != FS_TYPE_DIR || new_parent->unlinked) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    if (strcmp(new_basename, ".") == 0 || strcmp(new_basename, "..") == 0 ||
        strcmp(old_basename, ".") == 0 || strcmp(old_basename, "..") == 0) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }

    //moving a directory into itself or a descendant creates a parent/children cycle
    if (node->type == FS_TYPE_DIR) {
        tmpfs_node_t *anc = new_parent;
        while (anc) {
            if (anc == node) {
                spinlock_release(&tmpfs_lock);
                return -1;
            }
            anc = anc->parent;
        }
    }

    //save old name for rollback
    char saved_name[TMPFS_MAX_NAME];
    memcpy(saved_name, node->name, sizeof(saved_name));

    //remove from old parent
    for (uint32 i = 0; i < old_parent->dir.count; i++) {
        if (old_parent->dir.children[i] == node) {
            for (uint32 j = i; j < old_parent->dir.count - 1; j++) {
                old_parent->dir.children[j] = old_parent->dir.children[j + 1];
            }
            old_parent->dir.count--;
            break;
        }
    }

    //update name and parent (always NUL-terminated)
    strncpy(node->name, new_basename, TMPFS_MAX_NAME - 1);
    node->name[TMPFS_MAX_NAME - 1] = '\0';
    node->parent = new_parent;

    //add to new parent
    if (add_child(new_parent, node) != 0) {
        //rollback: restore old name and re-add to old parent
        memcpy(node->name, saved_name, sizeof(saved_name));
        node->parent = old_parent;
        add_child(old_parent, node);
        spinlock_release(&tmpfs_lock);
        return -1;
    }

    spinlock_release(&tmpfs_lock);
    return 0;
}

//recursively free a directory node and all its descendants
static void tmpfs_free_node_recursive(tmpfs_node_t *node) {
    if (!node) return;
    if (node->type == FS_TYPE_FILE) {
        kfree(node->file.data);
    } else if (node->type == FS_TYPE_DIR) {
        for (uint32 i = 0; i < node->dir.count; i++) {
            tmpfs_free_node_recursive(node->dir.children[i]);
        }
        kfree(node->dir.children);
    }
    kfree(node);
}

static int tmpfs_fs_remove(fs_t *fs, const char *path) {
    (void)fs;
    
    spinlock_acquire(&tmpfs_lock);
    
    tmpfs_node_t *parent = NULL;
    char basename[TMPFS_MAX_NAME];
    
    tmpfs_node_t *node = resolve_path(path, false, &parent, basename);
    if (!node || !parent) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    if (node == root) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    //POSIX: refuse to remove a non-empty directory instead of deleting a tree
    if (node->type == FS_TYPE_DIR && node->dir.count != 0) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    
    //remove from parent
    for (uint32 i = 0; i < parent->dir.count; i++) {
        if (parent->dir.children[i] == node) {
            //shift remaining
            for (uint32 j = i; j < parent->dir.count - 1; j++) {
                parent->dir.children[j] = parent->dir.children[j + 1];
            }
            parent->dir.count--;
            break;
        }
    }

    //unlink and defer free while open objects reference the node
    node->unlinked = true;
    if (node->refcount == 0) {
        tmpfs_free_node_recursive(node);
    }
    
    spinlock_release(&tmpfs_lock);
    return 0;
}

static int tmpfs_fs_stat(fs_t *fs, const char *path, stat_t *st) {
    (void)fs;
    if (!st) return -1;
    
    //hold lock while walking the tree - concurrent remove can UAF nodes
    spinlock_acquire(&tmpfs_lock);
    tmpfs_node_t *node = resolve_path(path, false, NULL, NULL);
    if (!node) {
        spinlock_release(&tmpfs_lock);
        return -1;
    }
    
    st->type = node->type;
    st->ctime = st->mtime = st->atime = node->ctime;
    
    if (node->type == FS_TYPE_FILE) {
        st->size = node->file.size;
    } else {
        st->size = 0;
    }
    spinlock_release(&tmpfs_lock);
    return 0;
}

static fs_ops_t tmpfs_ops = {
    .lookup = tmpfs_fs_lookup,
    .create = tmpfs_fs_create,
    .remove = tmpfs_fs_remove,
    .rename = tmpfs_fs_rename,
    .readdir = NULL,
    .stat = tmpfs_fs_stat
};

static fs_t tmpfs_instance = {
    .name = "tmpfs",
    .ops = &tmpfs_ops,
    .data = NULL
};

//root object
static ssize tmpfs_root_read(object_t *obj, void *buf, size len, size offset) {
    (void)obj; (void)buf; (void)len; (void)offset;
    return -1;
}

//root directory readdir - delegates to the actual tmpfs root node
static int tmpfs_root_readdir(object_t *obj, void *buf, uint32 count, uint32 *index) {
    (void)obj;
    if (!root || !buf || !index) return -1;
    
    //hold lock - concurrent create/remove can modify root->dir.children
    spinlock_acquire(&tmpfs_lock);
    dirent_t *entries = (dirent_t *)buf;
    uint32 start = *index;
    uint32 found = 0;
    
    for (uint32 i = start; i < root->dir.count && found < count; i++) {
        tmpfs_node_t *child = root->dir.children[i];
        if (child && !child->unlinked) {
            entries[found].type = child->type;
            strncpy(entries[found].name, child->name, DIRENT_NAME_MAX - 1);
            entries[found].name[DIRENT_NAME_MAX - 1] = '\0';
            found++;
        }
        *index = i + 1;
    }
    spinlock_release(&tmpfs_lock);
    return found;
}

static object_t *tmpfs_root_lookup(object_t *obj, const char *name) {
    fs_t *fs = (fs_t *)obj->data;
    if (!fs || !fs->ops || !fs->ops->lookup) return NULL;
    return fs->ops->lookup(fs, name);
}

static int tmpfs_root_stat(object_t *obj, stat_t *st) {
    (void)obj;
    if (!st) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DIR;
    return 0;
}

static object_ops_t tmpfs_root_ops = {
    .read = tmpfs_root_read,
    .write = NULL,
    .close = NULL,
    .readdir = tmpfs_root_readdir,
    .lookup = tmpfs_root_lookup,
    .stat = tmpfs_root_stat
};

static object_t *tmpfs_root_obj = NULL;

void tmpfs_init(void) {
    //create root directory
    root = kzalloc(sizeof(tmpfs_node_t));
    if (!root) {
        printf("[tmpfs] ERR: failed to allocate root\n");
        return;
    }
    
    root->name[0] = '\0';
    root->type = FS_TYPE_DIR;
    root->parent = NULL;
    root->refcount = 0;
    root->unlinked = false;
    root->dir.children = kzalloc(TMPFS_INITIAL_CHILDREN * sizeof(tmpfs_node_t *));
    if (!root->dir.children) {
        kfree(root);
        root = NULL;
        return;
    }
    root->dir.capacity = TMPFS_INITIAL_CHILDREN;
    root->dir.count = 0;
    
    tmpfs_root_obj = object_create(OBJECT_DIR, &tmpfs_root_ops, &tmpfs_instance);
    if (tmpfs_root_obj) {
        ns_register("$files", tmpfs_root_obj, HANDLE_RIGHTS_ALL);
    }
    
    puts("[tmpfs] initialized\n");
}

int tmpfs_create(const char *path) {
    return tmpfs_fs_create(&tmpfs_instance, path, FS_TYPE_FILE);
}

int tmpfs_create_dir(const char *path) {
    return tmpfs_fs_create(&tmpfs_instance, path, FS_TYPE_DIR);
}

object_t *tmpfs_open(const char *path) {
    return tmpfs_fs_lookup(&tmpfs_instance, path);
}
