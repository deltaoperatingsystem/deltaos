#include <obj/namespace.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/spinlock.h>
#include <fs/fs.h>

//namespace tree node
//each node holds a single path component like "keyboard"
typedef struct ns_node {
    char            *name; //component name (heap-allocated, NUL-terminated)
    object_t        *obj; //registered object; NULL for auto-created intermediates
    handle_rights_t  max_rights; //rights ceiling applied when the node is opened
    struct ns_node  *parent;
    struct ns_node  *first_child; //head of singly-linked children list
    struct ns_node  *next_sibling; //next node at the same level
} ns_node_t;

//hidden root sentinel - has no name or object, its children are the top-level entries
static ns_node_t  ns_root = {0};
static spinlock_t ns_lock = {0};

void ns_init(void) {
    //root is statically allocated; nothing to initialise
}

//find a direct child of parent matching the first len bytes of name
static ns_node_t *ns_find_child(ns_node_t *parent, const char *name, size len) {
    for (ns_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            return c;
    }
    return NULL;
}

//allocate and prepend a new child node under parent with the given name component
static ns_node_t *ns_create_child(ns_node_t *parent, const char *name, size len) {
    ns_node_t *node = kzalloc(sizeof(ns_node_t));
    if (!node) return NULL;

    node->name = kmalloc(len + 1);
    if (!node->name) { kfree(node); return NULL; }
    memcpy(node->name, name, len);
    node->name[len] = '\0';

    node->max_rights = HANDLE_RIGHTS_ALL;
    node->parent     = parent;

    //prepend to sibling list (order does not affect correctness)
    node->next_sibling = parent->first_child;
    parent->first_child = node;
    return node;
}

//walk path components split by '/' and optionally creating missing intermediate nodes
//returns the leaf node, or NULL if a component is missing (create=false) or OOM (create=true)
//must be called with ns_lock held
static ns_node_t *ns_walk(const char *path, bool create) {
    ns_node_t  *cur = &ns_root;
    const char *p   = path;

    while (*p) {
        //skip slashes
        if (*p == '/') { p++; continue; }

        //find end of this component
        const char *end = p;
        while (*end && *end != '/') end++;
        size len = (size)(end - p);
        if (!len) break;

        ns_node_t *child = ns_find_child(cur, p, len);
        if (!child) {
            if (!create) return NULL;
            child = ns_create_child(cur, p, len);
            if (!child) return NULL;
        }
        cur = child;
        p   = end;
    }

    //the root itself is not a valid return value (empty path)
    return (cur == &ns_root) ? NULL : cur;
}

//synthetic directory object for intermediate nodes
//created on demand when something opens a path like "$devices" or "$devices/disks"
//that has children but no explicitly registered object
static int ns_tree_dir_stat(object_t *obj, stat_t *st) {
    (void)obj;
    if (!st) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DIR;
    return 0;
}

static int ns_tree_dir_readdir(object_t *obj, void *entries_ptr, uint32 count, uint32 *index) {
    ns_node_t *node    = (ns_node_t *)obj->data;
    dirent_t  *entries = (dirent_t  *)entries_ptr;
    uint32     filled  = 0;
    uint32     skip    = *index;
    uint32     seen    = 0;

    spinlock_acquire(&ns_lock);
    for (ns_node_t *c = node->first_child; c && filled < count; c = c->next_sibling) {
        if (seen >= skip) {
            strncpy(entries[filled].name, c->name, sizeof(entries[filled].name) - 1);
            entries[filled].name[sizeof(entries[filled].name) - 1] = '\0';
            entries[filled].type = c->obj ? c->obj->type : OBJECT_DIR;
            filled++;
        }
        seen++;
    }
    spinlock_release(&ns_lock);

    *index = seen;
    return (int)filled;
}

static object_ops_t ns_tree_dir_ops = {
    .stat    = ns_tree_dir_stat,
    .readdir = ns_tree_dir_readdir,
};

//create a synthetic directory object pointing at the given ns_node_t
//must be called WITHOUT ns_lock held (object_create may allocate)
static object_t *ns_make_dir_obj(ns_node_t *node) {
    return object_create(OBJECT_NS_DIR, &ns_tree_dir_ops, node);
}

int ns_register(const char *name, object_t *obj, handle_rights_t max_rights) {
    if (!name || !obj) return -1;

    spinlock_acquire(&ns_lock);

    ns_node_t *node = ns_walk(name, /*create=*/true);
    if (!node) { spinlock_release(&ns_lock); return -1; }

    if (node->obj) {
        //already registered - refuse to overwrite silently
        spinlock_release(&ns_lock);
        return -1;
    }

    node->obj        = obj;
    node->max_rights = max_rights;
    object_ref(obj);

    spinlock_release(&ns_lock);
    return 0;
}

int ns_unregister(const char *name) {
    if (!name) return -1;

    spinlock_acquire(&ns_lock);

    ns_node_t *node = ns_walk(name, /*create=*/false);
    if (!node || !node->obj) {
        spinlock_release(&ns_lock);
        return -1;
    }

    object_deref(node->obj);
    node->obj        = NULL;
    node->max_rights = HANDLE_RIGHTS_ALL;

    spinlock_release(&ns_lock);
    return 0;
}

object_t *ns_lookup_ex(const char *name, handle_rights_t *max_rights_out) {
    if (!name) return NULL;

    spinlock_acquire(&ns_lock);

    ns_node_t  *cur = &ns_root;
    const char *p   = name;

    while (*p) {
        if (*p == '/') { p++; continue; }

        const char *end = p;
        while (*end && *end != '/') end++;
        size len = (size)(end - p);
        if (!len) break;

        ns_node_t *child = ns_find_child(cur, p, len);
        if (!child) {
            spinlock_release(&ns_lock);
            return NULL;
        }

        cur = child;
        p   = end;

        //skip any trailing slashes to find the true remaining path
        const char *remaining = p;
        while (*remaining == '/') remaining++;

        //if there is more path AND this node has a factory object with a lookup op
        //delegate the rest of the path to it (e.x $devices/keyboard -> "channel")
        //must release the lock before calling into factory code (may kmalloc/sleep)
        if (*remaining != '\0' && cur->obj && cur->obj->ops && cur->obj->ops->lookup) {
            object_t       *factory   = cur->obj;
            handle_rights_t ceiling   = cur->max_rights;
            object_ref(factory);
            spinlock_release(&ns_lock);

            object_t *result = factory->ops->lookup(factory, remaining);
            object_deref(factory);

            if (result && max_rights_out) *max_rights_out = ceiling;
            return result;
        }
    }

    //guard against the empty-path case (ns_walk would return NULL but we loop differently here)
    if (cur == &ns_root) {
        spinlock_release(&ns_lock);
        return NULL;
    }

    //reached the target node
    if (cur->obj) {
        object_t       *obj    = cur->obj;
        handle_rights_t rights = cur->max_rights;
        object_ref(obj);
        spinlock_release(&ns_lock);
        if (max_rights_out) *max_rights_out = rights;
        return obj;
    }

    //intermediate node with no registered object:
    //if it has children synthesise a directory object so callers can list it
    if (cur->first_child) {
        ns_node_t *node = cur;
        spinlock_release(&ns_lock);
        //allocate outside the lock (kzalloc not allowed under spinlock)
        object_t *dir_obj = ns_make_dir_obj(node);
        if (dir_obj && max_rights_out) *max_rights_out = HANDLE_RIGHTS_ALL;
        return dir_obj;
    }

    spinlock_release(&ns_lock);
    return NULL;
}

int ns_list(void *entries_ptr, uint32 count, uint32 *index) {
    if (!entries_ptr || !index) return -1;

    dirent_t *entries = (dirent_t *)entries_ptr;
    uint32    filled  = 0;
    uint32    skip    = *index;
    uint32    seen    = 0;

    spinlock_acquire(&ns_lock);
    for (ns_node_t *c = ns_root.first_child; c && filled < count; c = c->next_sibling) {
        if (seen >= skip) {
            strncpy(entries[filled].name, c->name, sizeof(entries[filled].name) - 1);
            entries[filled].name[sizeof(entries[filled].name) - 1] = '\0';
            entries[filled].type = c->obj ? c->obj->type : OBJECT_DIR;
            filled++;
        }
        seen++;
    }
    spinlock_release(&ns_lock);

    *index = seen;
    return (int)filled;
}
