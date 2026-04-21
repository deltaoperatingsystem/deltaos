#ifndef PROC_CONTEXT_H
#define PROC_CONTEXT_H

#include <arch/types.h>
#include <obj/object.h>
#include <obj/rights.h>
#include <lib/spinlock.h>

#define PROC_CONTEXT_KEY_MAX 96

//entry payload kinds for the per-process runtime context
typedef enum {
    PROC_CONTEXT_VALUE_STRING = 1,
    PROC_CONTEXT_VALUE_I64 = 2,
    PROC_CONTEXT_VALUE_U64 = 3,
    PROC_CONTEXT_VALUE_BOOL = 4,
    PROC_CONTEXT_VALUE_OBJECT = 5
} proc_context_value_type_t;

//flag set for the version of the context ABI
#define PROC_CONTEXT_FLAG_INHERIT   (1u << 0)
#define PROC_CONTEXT_FLAG_READONLY  (1u << 1)
#define PROC_CONTEXT_FLAG_VALID_MASK (PROC_CONTEXT_FLAG_INHERIT | PROC_CONTEXT_FLAG_READONLY)

typedef struct {
    object_t *obj;
    handle_rights_t rights;
} proc_context_object_t;

typedef struct {
    char *key;
    uint32 type;
    uint32 flags;
    union {
        char *str;
        int64 i64;
        uint64 u64;
        uint8 boolean;
        proc_context_object_t object;
    } value;
} proc_context_entry_t;

typedef struct {
    proc_context_entry_t *entries;
    size count;
    size capacity;
    spinlock_t lock;
} proc_context_t;

void proc_context_init(proc_context_t *ctx);
void proc_context_destroy(proc_context_t *ctx);
int proc_context_clone_for_child(proc_context_t *dst, const proc_context_t *src);

int proc_context_set_string(proc_context_t *ctx, const char *key, const char *value, uint32 flags);
int proc_context_set_i64(proc_context_t *ctx, const char *key, int64 value, uint32 flags);
int proc_context_set_u64(proc_context_t *ctx, const char *key, uint64 value, uint32 flags);
int proc_context_set_bool(proc_context_t *ctx, const char *key, int value, uint32 flags);
int proc_context_set_object(proc_context_t *ctx, const char *key, object_t *obj,
                            handle_rights_t rights, uint32 flags);
int proc_context_remove(proc_context_t *ctx, const char *key);
int proc_context_remove_force(proc_context_t *ctx, const char *key);

int proc_context_get_string_dup(const proc_context_t *ctx, const char *key, char **out_value,
                                size *out_len, uint32 *out_flags);
int proc_context_get_i64(const proc_context_t *ctx, const char *key, int64 *out_value, uint32 *out_flags);
int proc_context_get_u64(const proc_context_t *ctx, const char *key, uint64 *out_value, uint32 *out_flags);
int proc_context_get_bool(const proc_context_t *ctx, const char *key, int *out_value, uint32 *out_flags);
int proc_context_get_object_ref(const proc_context_t *ctx, const char *key, object_t **out_obj,
                                handle_rights_t *out_rights, uint32 *out_flags);

#endif
