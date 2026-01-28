#include <obj/namespace.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/io.h>
#include <fs/fs.h>

#define NS_INITIAL_BUCKETS 32
#define NS_LOAD_FACTOR_NUM 3
#define NS_LOAD_FACTOR_DEN 4  //rehash when 75% full

typedef struct ns_entry {
    char *name;
    object_t *obj;
    struct ns_entry *next;  //chaining for collisions
} ns_entry_t;

static ns_entry_t **buckets = NULL;
static uint32 bucket_count = 0;
static uint32 entry_count = 0;

//FNV-1a hash - fast and good distribution
static uint32 hash_string(const char *s) {
    uint32 hash = 2166136261u;
    while (*s) {
        hash ^= (uint8)*s++;
        hash *= 16777619u;
    }
    return hash;
}

static void ns_rehash(void) {
    uint32 new_count = bucket_count * 2;
    ns_entry_t **new_buckets = kzalloc(new_count * sizeof(ns_entry_t *));
    if (!new_buckets) return;  //keep old table if alloc fails
    
    //rehash all entries
    for (uint32 i = 0; i < bucket_count; i++) {
        ns_entry_t *entry = buckets[i];
        while (entry) {
            ns_entry_t *next = entry->next;
            uint32 new_idx = hash_string(entry->name) % new_count;
            entry->next = new_buckets[new_idx];
            new_buckets[new_idx] = entry;
            entry = next;
        }
    }
    
    kfree(buckets);
    buckets = new_buckets;
    bucket_count = new_count;
}

void ns_init(void) {
    buckets = kzalloc(NS_INITIAL_BUCKETS * sizeof(ns_entry_t *));
    if (!buckets) {
        printf("[namespace] ERR: failed to allocate hash table\n");
        return;
    }
    bucket_count = NS_INITIAL_BUCKETS;
    entry_count = 0;
}

static int ns_dir_stat(object_t *obj, stat_t *st) {
    (void)obj;
    if (!st) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DIR;
    return 0;
}

static object_t *ns_dir_lookup(object_t *obj, const char *name) {
    const char *prefix = (const char *)obj->data;
    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", prefix, name);
    return ns_lookup(full_path);
}

static int ns_dir_readdir(object_t *obj, void *entries_ptr, uint32 count, uint32 *index) {
    const char *prefix = (const char *)obj->data;
    size prefix_len = strlen(prefix);
    dirent_t *entries = (dirent_t *)entries_ptr;
    
    uint32 filled = 0;
    uint32 current_idx = 0;
    uint32 skip = *index;
    
    //iterate all buckets
    for (uint32 b = 0; b < bucket_count && filled < count; b++) {
        for (ns_entry_t *e = buckets[b]; e && filled < count; e = e->next) {
            //check if name starts with prefix
            if (strncmp(e->name, prefix, prefix_len) == 0) {
                //exclude the prefix itself if it was registered
                if (strlen(e->name) == prefix_len) {
                    current_idx++;
                    continue;
                }

                if (current_idx >= skip) {
                    const char *subname = e->name + prefix_len;
                    //handle nested directories by only returning the next component
                    const char *slash = strchr(subname, '/');
                    if (slash) {
                        //it's a "directory" in the flat namespace
                        size entry_len = slash - subname;
                        if (entry_len >= sizeof(entries[filled].name)) 
                            entry_len = sizeof(entries[filled].name) - 1;
                        
                        memcpy(entries[filled].name, subname, entry_len);
                        entries[filled].name[entry_len] = '\0';
                        entries[filled].type = OBJECT_DIR;
                    } else {
                        //it's a leaf node
                        strncpy(entries[filled].name, subname, sizeof(entries[filled].name) - 1);
                        entries[filled].name[sizeof(entries[filled].name) - 1] = '\0';
                        entries[filled].type = e->obj->type;
                    }
                    
                    //de-duplicate (if e.g. disks/nvme0n1 and disks/nvme0n2 both result in "disks")
                    bool duplicate = false;
                    for (uint32 j = 0; j < filled; j++) {
                        if (strcmp(entries[j].name, entries[filled].name) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    
                    if (!duplicate) {
                        filled++;
                    }
                }
                current_idx++;
            }
        }
    }
    
    *index = skip + filled;
    return filled;
}

static object_ops_t ns_dir_ops = {
    .readdir = ns_dir_readdir,
    .lookup = ns_dir_lookup,
    .read = NULL,
    .write = NULL,
    .stat = ns_dir_stat
};

object_t *ns_create_dir(const char *prefix) {
    return object_create(OBJECT_NS_DIR, &ns_dir_ops, (void *)prefix);
}

int ns_register(const char *name, object_t *obj) {
    if (!name || !obj || !buckets) return -1;
    
    //check load factor and rehash if needed
    if (entry_count * NS_LOAD_FACTOR_DEN >= bucket_count * NS_LOAD_FACTOR_NUM) {
        ns_rehash();
    }
    
    uint32 idx = hash_string(name) % bucket_count;
    
    //check if already exists
    for (ns_entry_t *e = buckets[idx]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            return -1;  //already exists
        }
    }
    
    //create new entry
    ns_entry_t *entry = kmalloc(sizeof(ns_entry_t));
    if (!entry) return -1;
    
    size name_len = strlen(name) + 1;
    entry->name = kmalloc(name_len);
    if (!entry->name) {
        kfree(entry);
        return -1;
    }
    
    memcpy(entry->name, name, name_len);
    entry->obj = obj;
    object_ref(obj);
    
    //insert at head of chain
    entry->next = buckets[idx];
    buckets[idx] = entry;
    entry_count++;
    
    return 0;
}

int ns_unregister(const char *name) {
    if (!name || !buckets) return -1;
    
    uint32 idx = hash_string(name) % bucket_count;
    ns_entry_t **prev = &buckets[idx];
    
    for (ns_entry_t *e = buckets[idx]; e; prev = &e->next, e = e->next) {
        if (strcmp(e->name, name) == 0) {
            *prev = e->next;
            object_deref(e->obj);
            kfree(e->name);
            kfree(e);
            entry_count--;
            return 0;
        }
    }
    return -1;  //not found
}

object_t *ns_lookup(const char *name) {
    if (!name || !buckets) return NULL;
    
    uint32 idx = hash_string(name) % bucket_count;
    
    for (ns_entry_t *e = buckets[idx]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            object_ref(e->obj);
            return e->obj;
        }
    }
    return NULL;
}

int ns_list(void *entries_ptr, uint32 count, uint32 *index) {
    if (!buckets || !entries_ptr || !index) return -1;
    
    dirent_t *entries = (dirent_t *)entries_ptr;
    uint32 filled = 0;
    uint32 skip = *index;
    uint32 seen = 0;
    
    //iterate all buckets and chains
    for (uint32 b = 0; b < bucket_count && filled < count; b++) {
        for (ns_entry_t *e = buckets[b]; e && filled < count; e = e->next) {
            if (seen >= skip) {
                //copy name into buffer
                strncpy(entries[filled].name, e->name, sizeof(entries[filled].name) - 1);
                entries[filled].name[sizeof(entries[filled].name) - 1] = '\0';
                entries[filled].type = e->obj->type;
                filled++;
            }
            seen++;
        }
    }
    
    *index = skip + filled;  //update index for next call
    return filled;
}
