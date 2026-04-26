#include <proc/context.h>
#include <mm/kheap.h>
#include <lib/string.h>

#define PROC_CONTEXT_INITIAL_CAPACITY 8

/*
 *process context is a kernel-owned key/value table that travels with a
 *process
  scalars are copied into the table directly, while object entries
 * retain an object reference plus the rights snapshot that should be used when
 * the entry is later materialized as a handle again
 */

//reject empty keys and unbounded names up front so every stored entry can be
//handled with fixed-size scratch buffers in the syscall layer
static int proc_context_validate_key(const char *key) {
    if (!key || !key[0]) return -1;
    if (strlen(key) >= PROC_CONTEXT_KEY_MAX) return -1;
    return 0;
}

//Free whatever payload the entry currently owns, then clear it back to zero
static void proc_context_release_entry(proc_context_entry_t *entry) {
    if (!entry) return;

    if (entry->type == PROC_CONTEXT_VALUE_STRING && entry->value.str) {
        kfree(entry->value.str);
    } else if (entry->type == PROC_CONTEXT_VALUE_OBJECT && entry->value.object.obj) {
        object_deref(entry->value.object.obj);
    }

    if (entry->key) {
        kfree(entry->key);
    }

    memset(entry, 0, sizeof(*entry));
}

static proc_context_entry_t *proc_context_find_locked(const proc_context_t *ctx, const char *key) {
    if (!ctx || !key) return NULL;

    for (size i = 0; i < ctx->count; i++) {
        if (ctx->entries[i].key && strcmp(ctx->entries[i].key, key) == 0) {
            return &ctx->entries[i];
        }
    }

    return NULL;
}

// Grow the backing array lazily so an empty context stays allocation-free.
static int proc_context_ensure_capacity_locked(proc_context_t *ctx) {
    if (ctx->count < ctx->capacity) return 0;

    size new_capacity = ctx->capacity ? (ctx->capacity * 2) : PROC_CONTEXT_INITIAL_CAPACITY;
    proc_context_entry_t *new_entries = krealloc(ctx->entries, new_capacity * sizeof(proc_context_entry_t));
    if (!new_entries) return -1;

    for (size i = ctx->capacity; i < new_capacity; i++) {
        memset(&new_entries[i], 0, sizeof(new_entries[i]));
    }

    ctx->entries = new_entries;
    ctx->capacity = new_capacity;
    return 0;
}

//reserve a slot for a key update while the context lock is held
//the table stays densely packed, so this helper either reuses the existing
//slot for the key or appends one fresh entry at the end of the array
static proc_context_entry_t *proc_context_reserve_entry_locked(proc_context_t *ctx, const char *key, uint32 flags) {
    proc_context_entry_t *entry = proc_context_find_locked(ctx, key);
    size key_len = strlen(key) + 1;
    char *key_copy;

    if (entry) {
        if (entry->flags & PROC_CONTEXT_FLAG_READONLY) {
            return NULL;
        }
    } else {
        if (proc_context_ensure_capacity_locked(ctx) != 0) return NULL;
    }

    key_copy = kmalloc(key_len);
    if (!key_copy) {
        return NULL;
    }
    memcpy(key_copy, key, key_len);

    if (entry) {
        proc_context_release_entry(entry);
    } else {
        entry = &ctx->entries[ctx->count++];
        memset(entry, 0, sizeof(*entry));
    }

    entry->key = key_copy;
    entry->flags = flags & PROC_CONTEXT_FLAG_VALID_MASK;
    return entry;
}

//shared scalar setter used by the i64/u64/bool entry helpers
//scalars are stored inline in the entry payload, so the only heap ownership
//involved here is the entry key itself
static int proc_context_set_scalar(proc_context_t *ctx, const char *key, uint32 type,
                                   const void *value, uint32 flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !value) return -1;
    if ((flags & ~PROC_CONTEXT_FLAG_VALID_MASK) != 0) return -1;

    spinlock_acquire(&ctx->lock);
    proc_context_entry_t *entry = proc_context_reserve_entry_locked(ctx, key, flags);
    if (!entry) {
        spinlock_release(&ctx->lock);
        return -1;
    }

    entry->type = type;
    if (type == PROC_CONTEXT_VALUE_I64) {
        entry->value.i64 = *(const int64 *)value;
    } else if (type == PROC_CONTEXT_VALUE_U64) {
        entry->value.u64 = *(const uint64 *)value;
    } else if (type == PROC_CONTEXT_VALUE_BOOL) {
        entry->value.boolean = *(const uint8 *)value ? 1 : 0;
    } else {
        proc_context_release_entry(entry);
        spinlock_release(&ctx->lock);
        return -1;
    }

    spinlock_release(&ctx->lock);
    return 0;
}

void proc_context_init(proc_context_t *ctx) {
    if (!ctx) return;

    ctx->entries = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    spinlock_init(&ctx->lock);
}

//destroy the entire table and release every heap/object payload it owns
void proc_context_destroy(proc_context_t *ctx) {
    if (!ctx) return;

    spinlock_acquire(&ctx->lock);
    for (size i = 0; i < ctx->count; i++) {
        proc_context_release_entry(&ctx->entries[i]);
    }

    if (ctx->entries) {
        kfree(ctx->entries);
    }

    ctx->entries = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    spinlock_release(&ctx->lock);
}

int proc_context_clone_for_child(proc_context_t *dst, const proc_context_t *src) {
    if (!dst || !src) return -1;

    //child creation only needs inherited entries non-inheritable entries stay
    //private to the current process and never appear in descendants
    spinlock_acquire((spinlock_t *)&src->lock);
    for (size i = 0; i < src->count; i++) {
        proc_context_entry_t *entry = &src->entries[i];
        int rc = 0;

        if (!entry->key || !(entry->flags & PROC_CONTEXT_FLAG_INHERIT)) {
            continue;
        }

        //clone through the public setters so copied entries keep exactly the
        //same ownership rules as if userspace had created them directly
        if (entry->type == PROC_CONTEXT_VALUE_STRING) {
            rc = proc_context_set_string(dst, entry->key, entry->value.str, entry->flags);
        } else if (entry->type == PROC_CONTEXT_VALUE_I64) {
            rc = proc_context_set_i64(dst, entry->key, entry->value.i64, entry->flags);
        } else if (entry->type == PROC_CONTEXT_VALUE_U64) {
            rc = proc_context_set_u64(dst, entry->key, entry->value.u64, entry->flags);
        } else if (entry->type == PROC_CONTEXT_VALUE_BOOL) {
            rc = proc_context_set_bool(dst, entry->key, entry->value.boolean != 0, entry->flags);
        } else if (entry->type == PROC_CONTEXT_VALUE_OBJECT) {
            rc = proc_context_set_object(dst, entry->key, entry->value.object.obj,
                                         entry->value.object.rights, entry->flags);
        } else {
            rc = -1;
        }

        if (rc != 0) {
            //a partial child context is not useful, roll it back completely so
            //callers either get a full inherited view or a hard failure
            spinlock_release((spinlock_t *)&src->lock);
            proc_context_destroy(dst);
            proc_context_init(dst);
            return -1;
        }
    }

    spinlock_release((spinlock_t *)&src->lock);
    return 0;
}

int proc_context_set_string(proc_context_t *ctx, const char *key, const char *value, uint32 flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !value) return -1;
    if ((flags & ~PROC_CONTEXT_FLAG_VALID_MASK) != 0) return -1;

    //strings are copied into kernel-owned memory so userspace can free or
    //mutate its source buffer immediately after the syscall returns
    size value_len = strlen(value) + 1;
    char *value_copy = kmalloc(value_len);
    if (!value_copy) return -1;
    memcpy(value_copy, value, value_len);

    spinlock_acquire(&ctx->lock);
    proc_context_entry_t *entry = proc_context_reserve_entry_locked(ctx, key, flags);
    if (!entry) {
        spinlock_release(&ctx->lock);
        kfree(value_copy);
        return -1;
    }

    entry->type = PROC_CONTEXT_VALUE_STRING;
    entry->value.str = value_copy;
    spinlock_release(&ctx->lock);
    return 0;
}

int proc_context_set_i64(proc_context_t *ctx, const char *key, int64 value, uint32 flags) {
    return proc_context_set_scalar(ctx, key, PROC_CONTEXT_VALUE_I64, &value, flags);
}

int proc_context_set_u64(proc_context_t *ctx, const char *key, uint64 value, uint32 flags) {
    return proc_context_set_scalar(ctx, key, PROC_CONTEXT_VALUE_U64, &value, flags);
}

int proc_context_set_bool(proc_context_t *ctx, const char *key, int value, uint32 flags) {
    uint8 boolean = value ? 1 : 0;
    return proc_context_set_scalar(ctx, key, PROC_CONTEXT_VALUE_BOOL, &boolean, flags);
}

int proc_context_set_object(proc_context_t *ctx, const char *key, object_t *obj,
                            handle_rights_t rights, uint32 flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !obj) return -1;
    if ((flags & ~PROC_CONTEXT_FLAG_VALID_MASK) != 0) return -1;

    spinlock_acquire(&ctx->lock);
    proc_context_entry_t *entry = proc_context_reserve_entry_locked(ctx, key, flags);
    if (!entry) {
        spinlock_release(&ctx->lock);
        return -1;
    }

    //object entries pin the underlying object and remember the exact rights
    //envelope that should be restored when turned back into a handle later
    object_ref(obj);
    entry->type = PROC_CONTEXT_VALUE_OBJECT;
    entry->value.object.obj = obj;
    entry->value.object.rights = rights;
    spinlock_release(&ctx->lock);
    return 0;
}

//remove one entry and compact the array so iteration order stays contiguous
static int proc_context_remove_impl(proc_context_t *ctx, const char *key, int ignore_readonly) {
    if (!ctx || proc_context_validate_key(key) != 0) return -1;

    spinlock_acquire(&ctx->lock);
    for (size i = 0; i < ctx->count; i++) {
        proc_context_entry_t *entry = &ctx->entries[i];
        if (!entry->key || strcmp(entry->key, key) != 0) {
            continue;
        }

        if (!ignore_readonly && (entry->flags & PROC_CONTEXT_FLAG_READONLY)) {
            spinlock_release(&ctx->lock);
            return -1;
        }

        proc_context_release_entry(entry);

        //keep the table densely packed so iteration and cloning stay simple
        for (size j = i + 1; j < ctx->count; j++) {
            ctx->entries[j - 1] = ctx->entries[j];
        }
        ctx->count--;
        if (ctx->count < ctx->capacity) {
            memset(&ctx->entries[ctx->count], 0, sizeof(ctx->entries[ctx->count]));
        }

        spinlock_release(&ctx->lock);
        return 0;
    }

    spinlock_release(&ctx->lock);
    return -1;
}

//remove one entry and respect the entry's read-only bit
int proc_context_remove(proc_context_t *ctx, const char *key) {
    return proc_context_remove_impl(ctx, key, 0);
}

//spawn-time overrides need a privileged path that can replace inherited
//read-only entries before the child starts executing in userspace
int proc_context_remove_force(proc_context_t *ctx, const char *key) {
    return proc_context_remove_impl(ctx, key, 1);
}

//string getters duplicate the payload because syscall code needs to drop the
//context lock before copying bytes back into userspace
int proc_context_get_string_dup(const proc_context_t *ctx, const char *key, char **out_value,
                                size *out_len, uint32 *out_flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !out_value) return -1;

    spinlock_acquire((spinlock_t *)&ctx->lock);
    proc_context_entry_t *entry = proc_context_find_locked(ctx, key);
    if (!entry || entry->type != PROC_CONTEXT_VALUE_STRING) {
        spinlock_release((spinlock_t *)&ctx->lock);
        return -1;
    }

    //getters hand back a detached copy so callers can drop the lock before
    //exposing the data to other kernel subsystems or to userspace
    size len = strlen(entry->value.str) + 1;
    char *copy = kmalloc(len);
    if (!copy) {
        spinlock_release((spinlock_t *)&ctx->lock);
        return -1;
    }

    memcpy(copy, entry->value.str, len);
    if (out_len) *out_len = len;
    if (out_flags) *out_flags = entry->flags;
    *out_value = copy;
    spinlock_release((spinlock_t *)&ctx->lock);
    return 0;
}

//scalar getters just copy the inline value out while the context is locked
int proc_context_get_i64(const proc_context_t *ctx, const char *key, int64 *out_value, uint32 *out_flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !out_value) return -1;

    spinlock_acquire((spinlock_t *)&ctx->lock);
    proc_context_entry_t *entry = proc_context_find_locked(ctx, key);
    if (!entry || entry->type != PROC_CONTEXT_VALUE_I64) {
        spinlock_release((spinlock_t *)&ctx->lock);
        return -1;
    }

    *out_value = entry->value.i64;
    if (out_flags) *out_flags = entry->flags;
    spinlock_release((spinlock_t *)&ctx->lock);
    return 0;
}

int proc_context_get_u64(const proc_context_t *ctx, const char *key, uint64 *out_value, uint32 *out_flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !out_value) return -1;

    spinlock_acquire((spinlock_t *)&ctx->lock);
    proc_context_entry_t *entry = proc_context_find_locked(ctx, key);
    if (!entry || entry->type != PROC_CONTEXT_VALUE_U64) {
        spinlock_release((spinlock_t *)&ctx->lock);
        return -1;
    }

    *out_value = entry->value.u64;
    if (out_flags) *out_flags = entry->flags;
    spinlock_release((spinlock_t *)&ctx->lock);
    return 0;
}

int proc_context_get_bool(const proc_context_t *ctx, const char *key, int *out_value, uint32 *out_flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !out_value) return -1;

    spinlock_acquire((spinlock_t *)&ctx->lock);
    proc_context_entry_t *entry = proc_context_find_locked(ctx, key);
    if (!entry || entry->type != PROC_CONTEXT_VALUE_BOOL) {
        spinlock_release((spinlock_t *)&ctx->lock);
        return -1;
    }

    *out_value = entry->value.boolean ? 1 : 0;
    if (out_flags) *out_flags = entry->flags;
    spinlock_release((spinlock_t *)&ctx->lock);
    return 0;
}

//object getters hand back a fresh object reference so callers can materialize
//a handle or inspect the object after the context lock has been released
int proc_context_get_object_ref(const proc_context_t *ctx, const char *key, object_t **out_obj,
                                handle_rights_t *out_rights, uint32 *out_flags) {
    if (!ctx || proc_context_validate_key(key) != 0 || !out_obj || !out_rights) return -1;

    spinlock_acquire((spinlock_t *)&ctx->lock);
    proc_context_entry_t *entry = proc_context_find_locked(ctx, key);
    if (!entry || entry->type != PROC_CONTEXT_VALUE_OBJECT || !entry->value.object.obj) {
        spinlock_release((spinlock_t *)&ctx->lock);
        return -1;
    }

    //callers receive a new ref so they can safely mint handles or inspect the
    //object after dropping the context lock
    object_ref(entry->value.object.obj);
    *out_obj = entry->value.object.obj;
    *out_rights = entry->value.object.rights;
    if (out_flags) *out_flags = entry->flags;
    spinlock_release((spinlock_t *)&ctx->lock);
    return 0;
}
