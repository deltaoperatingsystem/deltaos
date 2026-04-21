#include <syscall/syscall.h>
#include <proc/process.h>
#include <proc/context.h>
#include <mm/kheap.h>
#include <lib/string.h>

#define CONTEXT_MAX_STRING 4096

static int copy_context_key(const char *user_key, char key[PROC_CONTEXT_KEY_MAX]) {
    if (!user_key) return -1;
    return copy_user_cstr(user_key, key, PROC_CONTEXT_KEY_MAX);
}

static int copy_context_flags(uint32 *user_flags, uint32 flags) {
    if (!user_flags) return 0;
    return copy_to_user_bytes(user_flags, &flags, sizeof(flags));
}

intptr sys_context_set(const char *key, uint32 type, const void *value_ptr, size value_len, uint32 flags) {
    process_t *proc = process_current();
    char k_key[PROC_CONTEXT_KEY_MAX];

    if (!proc) return -1;
    if (copy_context_key(key, k_key) != 0) return -1;

    if (type == CONTEXT_VALUE_STRING) {
        if (!value_ptr || value_len == 0 || value_len > CONTEXT_MAX_STRING) return -1;

        char *tmp = kmalloc(value_len);
        if (!tmp) return -1;

        if (copy_user_bytes(value_ptr, tmp, value_len) != 0) {
            kfree(tmp);
            return -1;
        }
        if (tmp[value_len - 1] != '\0') {
            kfree(tmp);
            return -1;
        }

        int rc = proc_context_set_string(&proc->context, k_key, tmp, flags);
        kfree(tmp);
        return rc;
    }

    if (type == CONTEXT_VALUE_I64) {
        int64 value;
        if (!value_ptr || value_len != sizeof(value)) return -1;
        if (copy_user_bytes(value_ptr, &value, sizeof(value)) != 0) return -1;
        return proc_context_set_i64(&proc->context, k_key, value, flags);
    }

    if (type == CONTEXT_VALUE_U64) {
        uint64 value;
        if (!value_ptr || value_len != sizeof(value)) return -1;
        if (copy_user_bytes(value_ptr, &value, sizeof(value)) != 0) return -1;
        return proc_context_set_u64(&proc->context, k_key, value, flags);
    }

    if (type == CONTEXT_VALUE_BOOL) {
        uint32 value;
        if (!value_ptr || value_len != sizeof(value)) return -1;
        if (copy_user_bytes(value_ptr, &value, sizeof(value)) != 0) return -1;
        return proc_context_set_bool(&proc->context, k_key, value != 0, flags);
    }

    return -1;
}

intptr sys_context_get(const char *key, uint32 type, void *value_ptr, size value_len, uint32 *flags_out) {
    process_t *proc = process_current();
    char k_key[PROC_CONTEXT_KEY_MAX];
    uint32 flags = 0;

    if (!proc) return -1;
    if (copy_context_key(key, k_key) != 0) return -1;

    if (type == CONTEXT_VALUE_STRING) {
        char *tmp = NULL;
        size len = 0;

        if (proc_context_get_string_dup(&proc->context, k_key, &tmp, &len, &flags) != 0) {
            return -1;
        }

        if (!value_ptr || value_len < len) {
            kfree(tmp);
            return -1;
        }

        if (copy_to_user_bytes(value_ptr, tmp, len) != 0) {
            kfree(tmp);
            return -1;
        }

        kfree(tmp);
        if (copy_context_flags(flags_out, flags) != 0) return -1;
        return (intptr)len;
    }

    if (type == CONTEXT_VALUE_I64) {
        int64 value;
        if (!value_ptr || value_len < sizeof(value)) return -1;
        if (proc_context_get_i64(&proc->context, k_key, &value, &flags) != 0) return -1;
        if (copy_to_user_bytes(value_ptr, &value, sizeof(value)) != 0) return -1;
        if (copy_context_flags(flags_out, flags) != 0) return -1;
        return 0;
    }

    if (type == CONTEXT_VALUE_U64) {
        uint64 value;
        if (!value_ptr || value_len < sizeof(value)) return -1;
        if (proc_context_get_u64(&proc->context, k_key, &value, &flags) != 0) return -1;
        if (copy_to_user_bytes(value_ptr, &value, sizeof(value)) != 0) return -1;
        if (copy_context_flags(flags_out, flags) != 0) return -1;
        return 0;
    }

    if (type == CONTEXT_VALUE_BOOL) {
        uint32 value = 0;
        int boolean = 0;
        if (!value_ptr || value_len < sizeof(value)) return -1;
        if (proc_context_get_bool(&proc->context, k_key, &boolean, &flags) != 0) return -1;
        value = boolean ? 1u : 0u;
        if (copy_to_user_bytes(value_ptr, &value, sizeof(value)) != 0) return -1;
        if (copy_context_flags(flags_out, flags) != 0) return -1;
        return 0;
    }

    return -1;
}

intptr sys_context_set_handle(const char *key, handle_t h, uint32 flags) {
    process_t *proc = process_current();
    char k_key[PROC_CONTEXT_KEY_MAX];
    proc_handle_t *entry;

    if (!proc) return -1;
    if (copy_context_key(key, k_key) != 0) return -1;

    entry = process_get_handle_entry(proc, h);
    if (!entry) return -1;

    //the context stores the object plus its rights snapshot so inheritance
    //keeps the same capability envelope without eagerly minting child handles
    return proc_context_set_object(&proc->context, k_key, entry->obj, entry->rights, flags);
}

intptr sys_context_get_handle(const char *key, handle_t *out_h, uint32 *flags_out) {
    process_t *proc = process_current();
    char k_key[PROC_CONTEXT_KEY_MAX];
    object_t *obj = NULL;
    handle_rights_t rights = HANDLE_RIGHT_NONE;
    uint32 flags = 0;
    int new_handle;

    if (!proc || !out_h) return -1;
    if (copy_context_key(key, k_key) != 0) return -1;

    if (proc_context_get_object_ref(&proc->context, k_key, &obj, &rights, &flags) != 0) {
        return -1;
    }

    new_handle = process_grant_handle(proc, obj, rights);
    object_deref(obj);
    if (new_handle < 0) return -1;

    if (copy_context_flags(flags_out, flags) != 0) {
        process_close_handle(proc, new_handle);
        return -1;
    }
    if (copy_to_user_bytes(out_h, &new_handle, sizeof(new_handle)) != 0) {
        process_close_handle(proc, new_handle);
        return -1;
    }

    return 0;
}

intptr sys_context_remove(const char *key) {
    process_t *proc = process_current();
    char k_key[PROC_CONTEXT_KEY_MAX];

    if (!proc) return -1;
    if (copy_context_key(key, k_key) != 0) return -1;
    return proc_context_remove(&proc->context, k_key);
}
