#include <system.h>
#include <sys/syscall.h>
#include <string.h>

int context_set_string(const char *key, const char *value, uint32 flags) {
    if (!value) return -1;
    return (int)__syscall5(SYS_CONTEXT_SET, (long)key, CONTEXT_VALUE_STRING,
                           (long)value, (long)(strlen(value) + 1), (long)flags);
}

int context_set_i64(const char *key, int64 value, uint32 flags) {
    return (int)__syscall5(SYS_CONTEXT_SET, (long)key, CONTEXT_VALUE_I64,
                           (long)&value, sizeof(value), (long)flags);
}

int context_set_u64(const char *key, uint64 value, uint32 flags) {
    return (int)__syscall5(SYS_CONTEXT_SET, (long)key, CONTEXT_VALUE_U64,
                           (long)&value, sizeof(value), (long)flags);
}

int context_set_bool(const char *key, int value, uint32 flags) {
    uint32 boolean = value ? 1u : 0u;
    return (int)__syscall5(SYS_CONTEXT_SET, (long)key, CONTEXT_VALUE_BOOL,
                           (long)&boolean, sizeof(boolean), (long)flags);
}

int context_set_handle(const char *key, handle_t h, uint32 flags) {
    return (int)__syscall3(SYS_CONTEXT_SET_HANDLE, (long)key, (long)h, (long)flags);
}

int context_get_string(const char *key, char *buf, size bufsize, uint32 *flags_out) {
    return (int)__syscall5(SYS_CONTEXT_GET, (long)key, CONTEXT_VALUE_STRING,
                           (long)buf, (long)bufsize, (long)flags_out);
}

int context_get_i64(const char *key, int64 *out_value, uint32 *flags_out) {
    return (int)__syscall5(SYS_CONTEXT_GET, (long)key, CONTEXT_VALUE_I64,
                           (long)out_value, sizeof(*out_value), (long)flags_out);
}

int context_get_u64(const char *key, uint64 *out_value, uint32 *flags_out) {
    return (int)__syscall5(SYS_CONTEXT_GET, (long)key, CONTEXT_VALUE_U64,
                           (long)out_value, sizeof(*out_value), (long)flags_out);
}

int context_get_bool(const char *key, int *out_value, uint32 *flags_out) {
    uint32 boolean = 0;
    int rc = (int)__syscall5(SYS_CONTEXT_GET, (long)key, CONTEXT_VALUE_BOOL,
                             (long)&boolean, sizeof(boolean), (long)flags_out);
    if (rc < 0) return rc;
    if (out_value) *out_value = boolean ? 1 : 0;
    return 0;
}

int context_get_handle(const char *key, handle_t *out_h, uint32 *flags_out) {
    return (int)__syscall3(SYS_CONTEXT_GET_HANDLE, (long)key, (long)out_h, (long)flags_out);
}

int context_remove(const char *key) {
    return (int)__syscall1(SYS_CONTEXT_REMOVE, (long)key);
}
