#include <fs/tmpfs.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>

#define TMPFS_MAX_NAME 64
#define TMPFS_INITIAL_BUF 256
#define TMPFS_INITIAL_CHILDREN 8

//tmpfs node (file or directory)
typedef struct tmpfs_node {
    char name[TMPFS_MAX_NAME];
    uint32 type;  //FS_TYPE_FILE or FS_TYPE_DIR
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
    if (offset >= node->file.size) return 0;
    
    size avail = node->file.size - offset;
    size to_read = len < avail ? len : avail;
    memcpy(buf, node->file.data + offset, to_read);
    return to_read;
}

//file object write
static ssize tmpfs_file_write(object_t *obj, const void *buf, size len, size offset) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_FILE) return -1;
    
    size end = offset + len;
    
    if (end > node->file.capacity) {
        size new_cap = node->file.capacity ? node->file.capacity * 2 : TMPFS_INITIAL_BUF;
        while (new_cap < end) new_cap *= 2;
        
        uint8 *new_data = krealloc(node->file.data, new_cap);
        if (!new_data) return -1;
        
        node->file.data = new_data;
        node->file.capacity = new_cap;
    }
    
    memcpy(node->file.data + offset, buf, len);
    if (end > node->file.size) node->file.size = end;
    
    return len;
}

static object_ops_t tmpfs_file_ops = {
    .read = tmpfs_file_read,
    .write = tmpfs_file_write,
    .close = NULL,
    .ioctl = NULL,
    .readdir = NULL
};

//directory object readdir
static int tmpfs_dir_readdir(object_t *obj, void *buf, uint32 count, uint32 *index) {
    tmpfs_node_t *node = (tmpfs_node_t *)obj->data;
    if (!node || node->type != FS_TYPE_DIR) return -1;
    
    dirent_t *entries = (dirent_t *)buf;
    uint32 start = *index;
    uint32 filled = 0;
    
    for (uint32 i = start; i < node->dir.count && filled < count; i++) {
        entries[filled].name = node->dir.children[i]->name;
        entries[filled].type = node->dir.children[i]->type;
        filled++;
    }
    
    *index = start + filled;
    return filled;
}

static object_ops_t tmpfs_dir_ops = {
    .read = NULL,
    .write = NULL,
    .close = NULL,
    .ioctl = NULL,
    .readdir = tmpfs_dir_readdir
};

//filesystem ops
static object_t *tmpfs_fs_lookup(fs_t *fs, const char *path) {
    (void)fs;
    tmpfs_node_t *node = resolve_path(path, false, NULL, NULL);
    if (!node) return NULL;
    
    if (node->type == FS_TYPE_FILE) {
        return object_create(OBJECT_FILE, &tmpfs_file_ops, node);
    } else if (node->type == FS_TYPE_DIR) {
        return object_create(OBJECT_DIR, &tmpfs_dir_ops, node);
    }
    return NULL;
}

static int tmpfs_fs_create(fs_t *fs, const char *path, uint32 type) {
    (void)fs;
    
    tmpfs_node_t *parent = NULL;
    char basename[TMPFS_MAX_NAME];
    
    //check if already exists
    if (resolve_path(path, false, NULL, NULL)) return -1;
    
    //resolve path and create parent dirs
    resolve_path(path, true, &parent, basename);
    if (!parent || parent->type != FS_TYPE_DIR) return -1;
    
    //create new node
    tmpfs_node_t *node = kzalloc(sizeof(tmpfs_node_t));
    if (!node) return -1;
    
    strncpy(node->name, basename, TMPFS_MAX_NAME - 1);
    node->type = type;
    
    if (type == FS_TYPE_DIR) {
        node->dir.children = kzalloc(TMPFS_INITIAL_CHILDREN * sizeof(tmpfs_node_t *));
        if (!node->dir.children) { kfree(node); return -1; }
        node->dir.capacity = TMPFS_INITIAL_CHILDREN;
        node->dir.count = 0;
    } else {
        node->file.data = NULL;
        node->file.size = 0;
        node->file.capacity = 0;
    }
    
    return add_child(parent, node);
}

static int tmpfs_fs_remove(fs_t *fs, const char *path) {
    (void)fs;
    tmpfs_node_t *parent = NULL;
    char basename[TMPFS_MAX_NAME];
    
    tmpfs_node_t *node = resolve_path(path, false, &parent, basename);
    if (!node || !parent) return -1;
    
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
    
    return 0;
}

static fs_ops_t tmpfs_ops = {
    .lookup = tmpfs_fs_lookup,
    .create = tmpfs_fs_create,
    .remove = tmpfs_fs_remove
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

static object_ops_t tmpfs_root_ops = {
    .read = tmpfs_root_read,
    .write = NULL,
    .close = NULL,
    .ioctl = NULL
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
