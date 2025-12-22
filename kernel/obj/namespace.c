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
                //point to name (caller must not modify or free)
                entries[filled].name = e->name;
                entries[filled].type = e->obj->type;
                filled++;
            }
            seen++;
        }
    }
    
    *index = skip + filled;  //update index for next call
    return filled;
}
