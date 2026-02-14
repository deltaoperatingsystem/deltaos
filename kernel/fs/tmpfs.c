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

//find child by name in directory
static tmpfs_node_t *find_child(tmpfs_node_t *dir, const char *name) {
    if (!dir || dir->type != FS_TYPE_DIR) return NULL;
    
    for (uint32 i = 0; i < dir->dir.count; i++) {
        if (strcmp(dir->dir.children[i]->name, name) == 0) {
            return dir->dir.children[i];
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
            next_node->type = FS_TYPE_DIR;
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
    
    size end = offset + len;
    
    if (end > node->file.capacity) {
        size new_cap = node->file.capacity ? node->file.capacity * 2 : TMPFS_INITIAL_BUF;
        while (new_cap < end) new_cap *= 2;
        
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
    st->type = FS_TYPE_FILE;
    st->size = node->file.size;
    st->ctime = st->mtime = st->atime = node->ctime;
    return 0;
}

static object_ops_t tmpfs_file_ops = {
    .read = tmpfs_file_read,
    .write = tmpfs_file_write,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = tmpfs_file_stat
};

//directory object readdir
static int tmpfs_dir_readdir(object_t *obj, void *buf, uint32 count, uint32 *index) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_DIR) return -1;
    
    dirent_t *entries = (dirent_t *)buf;
    uint32 start = *index;
    uint32 filled = 0;
    
    for (uint32 i = start; i < node->dir.count && filled < count; i++) {
        strncpy(entries[filled].name, node->dir.children[i]->name, sizeof(entries[filled].name) - 1);
        entries[filled].name[sizeof(entries[filled].name) - 1] = '\0';
        entries[filled].type = node->dir.children[i]->type;
        filled++;
    }
    
    *index = start + filled;
    return filled;
}

//forward declaration for recursive reference
static object_ops_t tmpfs_dir_ops;

static int tmpfs_dir_stat(object_t *obj, stat_t *st) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || !st) return -1;
    st->type = FS_TYPE_DIR;
    st->size = 0;
    st->ctime = st->mtime = st->atime = node->ctime;
    return 0;
}

//directory lookup - find child by name
static object_t *tmpfs_dir_lookup(object_t *obj, const char *name) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_DIR) return NULL;
    
    tmpfs_node_t *child = find_child(node, name);
    if (!child) return NULL;
    
    //create object for the child
    if (child->type == FS_TYPE_FILE) {
        return object_create(OBJECT_FILE, &tmpfs_file_ops, child);
    } else if (child->type == FS_TYPE_DIR) {
        return object_create(OBJECT_DIR, &tmpfs_dir_ops, child);
    }
    return NULL;
}

static object_ops_t tmpfs_dir_ops = {
    .read = NULL,
    .write = NULL,
    .close = NULL,
    .readdir = tmpfs_dir_readdir,
    .lookup = tmpfs_dir_lookup,
    .stat = tmpfs_dir_stat
};

//filesystem ops
static object_t *tmpfs_fs_lookup(fs_t *fs, const char *path) {
    (void)fs;
    spinlock_acquire(&tmpfs_lock);
    tmpfs_node_t *node = resolve_path(path, false, NULL, NULL);
    if (!node) {
        spinlock_release(&tmpfs_lock);
        return NULL;
    }
    
    object_t *obj = NULL;
    if (node->type == FS_TYPE_FILE) {
        obj = object_create(OBJECT_FILE, &tmpfs_file_ops, node);
    } else if (node->type == FS_TYPE_DIR) {
        obj = object_create(OBJECT_DIR, &tmpfs_dir_ops, node);
    }
    spinlock_release(&tmpfs_lock);
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
    node->type = type;
    node->ctime = get_current_time();
    
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
    
    int result = add_child(parent, node);
    spinlock_release(&tmpfs_lock);
    return result;
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
    
    //free node data
    if (node->type == FS_TYPE_FILE) {
        kfree(node->file.data);
    } else {
        kfree(node->dir.children);
    }
    kfree(node);
    
    spinlock_release(&tmpfs_lock);
    return 0;
}

static int tmpfs_fs_stat(fs_t *fs, const char *path, stat_t *st) {
    (void)fs;
    if (!st) return -1;
    
    tmpfs_node_t *node = resolve_path(path, false, NULL, NULL);
    if (!node) return -1;
    
    st->type = node->type;
    st->ctime = st->mtime = st->atime = node->ctime;
    
    if (node->type == FS_TYPE_FILE) {
        st->size = node->file.size;
    } else {
        st->size = 0;
    }
    
    return 0;
}

static fs_ops_t tmpfs_ops = {
    .lookup = tmpfs_fs_lookup,
    .create = tmpfs_fs_create,
    .remove = tmpfs_fs_remove,
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
    
    dirent_t *entries = (dirent_t *)buf;
    uint32 start = *index;
    uint32 found = 0;
    
    for (uint32 i = start; i < root->dir.count && found < count; i++) {
        tmpfs_node_t *child = root->dir.children[i];
        if (child) {
            entries[found].type = child->type;
            strncpy(entries[found].name, child->name, DIRENT_NAME_MAX - 1);
            entries[found].name[DIRENT_NAME_MAX - 1] = '\0';
            found++;
            *index = i + 1;
        }
    }
    
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
        ns_register("$files", tmpfs_root_obj);
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
