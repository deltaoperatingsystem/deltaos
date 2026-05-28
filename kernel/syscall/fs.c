#include <syscall/syscall.h>
#include <fs/fs.h>
#include <proc/process.h>
#include <lib/string.h>
#include <lib/path.h>
#include <lib/io.h>
#include <fs/fat32.h>
#include <fs/mount.h>
#include <arch/percpu.h>
#include <mm/kheap.h>
#include <errno.h>
#include <atomic.h>

#define MAX_HANDLE_IO (1u << 20)

intptr sys_handle_read(handle_t h, void *buf, size len) {
    if (!buf || len == 0) return -1;

    intptr total = 0;
    uint8 *ubuf = (uint8*)buf;

    while (len > 0) {
        size chunk = len > MAX_HANDLE_IO ? MAX_HANDLE_IO : len;

        void *kbuf = kmalloc(chunk);
        if (!kbuf) return total > 0 ? total : -1;

        ssize r = handle_read(h, kbuf, chunk);
        if (r <= 0) {
            kfree(kbuf);
            return total > 0 ? total : r;
        }

        if (copy_to_user_bytes(ubuf + total, kbuf, (size)r) != 0) {
            kfree(kbuf);
            return total > 0 ? total : -1;
        }

        kfree(kbuf);
        total += r;
        if ((size)r < chunk) break;
        len -= r;
    }

    return total;
}

intptr sys_handle_write(handle_t h, const void *buf, size len) {
    if (!buf || len == 0) return -1;

    intptr total = 0;
    const uint8 *ubuf = (const uint8*)buf;

    while (len > 0) {
        size chunk = len > MAX_HANDLE_IO ? MAX_HANDLE_IO : len;

        void *kbuf = kmalloc(chunk);
        if (!kbuf) return total > 0 ? total : -1;

        if (copy_user_bytes(ubuf + total, kbuf, chunk) != 0) {
            kfree(kbuf);
            return total > 0 ? total : -1;
        }

        intptr r = handle_write(h, kbuf, chunk);
        kfree(kbuf);

        if (r <= 0) {
            return total > 0 ? total : r;
        }

        total += r;
        if ((size)r < chunk) break;
        len -= r;
    }

    return total;
}

intptr sys_handle_seek(handle_t h, size offset, int mode) {
    return handle_seek(h, offset, mode);
}

intptr sys_stat(const char *path, stat_t *st) {
    //copy path and stat result through kernel buffers
    if (!path || !st) return -1;
    char k_path[512];
    if (copy_user_cstr(path, k_path, sizeof(k_path)) != 0) return -1;
    stat_t k_st;
    int ret = handle_stat(k_path, &k_st);
    if (ret != 0) return ret;
    if (copy_to_user_bytes(st, &k_st, sizeof(k_st)) != 0) return -1;
    return 0;
}

intptr sys_fstat(handle_t h, stat_t *st) {
    //copy stat result through kernel buffer
    if (!st) return -1;
    stat_t k_st;
    int ret = handle_fstat(h, &k_st);
    if (ret != 0) return ret;
    if (copy_to_user_bytes(st, &k_st, sizeof(k_st)) != 0) return -1;
    return 0;
}

intptr sys_readdir(handle_t h, dirent_t *entries, uint32 count, uint32 *index) {
    //copy entries and index through kernel buffers
    if (!entries || !index || count == 0) return -1;

    uint32 k_index = 0;
    if (copy_user_bytes(index, &k_index, sizeof(k_index)) != 0) return -1;

    //cap to avoid a huge kernel allocation
    if (count > 256) count = 256;
    dirent_t *k_entries = kzalloc(count * sizeof(dirent_t));
    if (!k_entries) return -1;

    process_t *proc = process_current();
    if (!proc) {
        kfree(k_entries);
        return -1;
    }

    spinlock_acquire(&proc->lock);
    proc_handle_t *entry = process_get_handle_entry(proc, h);
    if (entry) entry->offset = k_index;
    spinlock_release(&proc->lock);

    int result = handle_readdir(h, k_entries, count);
    if (result < 0) {
        kfree(k_entries);
        return result;
    }

    //handle_readdir updates the handle's internal offset; read it back
    spinlock_acquire(&proc->lock);
    entry = process_get_handle_entry(proc, h);
    if (entry) k_index = (uint32)entry->offset;
    spinlock_release(&proc->lock);

    if (copy_to_user_bytes(entries, k_entries, (size)result * sizeof(dirent_t)) != 0) {
        kfree(k_entries);
        return -1;
    }
    if (copy_to_user_bytes(index, &k_index, sizeof(k_index)) != 0) {
        kfree(k_entries);
        return -1;
    }
    kfree(k_entries);
    return result;
}

intptr sys_chdir(const char *path) {
    //copy path via kernel buffer
    if (!path) return -1;

    char k_path[512];
    if (copy_user_cstr(path, k_path, sizeof(k_path)) != 0) return -1;

    process_t *proc = process_current();
    if (!proc) return -1;

    if (k_path[0] == '$') return -4;

    stat_t st;
    if (handle_stat(k_path, &st) != 0) return -2;
    if (st.type != FS_TYPE_DIR) return -3;

    char full_path[256];
    if (k_path[0] == '/') {
        strncpy(full_path, k_path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", proc->cwd, k_path);
    }

    path_normalize(full_path);

    size final_len = strlen(full_path);
    if (final_len >= sizeof(proc->cwd)) return -1;

    memcpy(proc->cwd, full_path, final_len + 1);
    return 0;
}

intptr sys_getcwd(char *buf, size bufsize) {
    //copy cwd through copy_to_user_bytes
    if (!buf || bufsize == 0) return -1;

    process_t *proc = process_current();
    if (!proc) return -1;

    size cwd_len = strlen(proc->cwd);
    if (cwd_len >= bufsize) return -1;

    if (copy_to_user_bytes(buf, proc->cwd, cwd_len + 1) != 0) return -1;
    return (intptr)cwd_len;
}

intptr sys_mount(handle_t source, const char *target, const char *fstype) {
    if (!target || !fstype) return -1;

    //mounting needs raw read/write access plus metadata access
    if (!handle_has_rights(source, HANDLE_RIGHT_READ) ||
        !handle_has_rights(source, HANDLE_RIGHT_WRITE) ||
        !handle_has_rights(source, HANDLE_RIGHT_GET_INFO)) {
        return -2;
    }

    char k_target[256];
    char k_type[64];
    if (copy_user_cstr(target, k_target, sizeof(k_target)) != 0) return -1;
    if (copy_user_cstr(fstype, k_type, sizeof(k_type)) != 0) return -1;

    if (k_target[0] != '/') return -3;
    path_normalize(k_target);
    if (strcmp(k_target, "/") == 0) return -3;

    //only mount onto an existing directory
    stat_t st;
    if (handle_stat(k_target, &st) != 0 || st.type != FS_TYPE_DIR) {
        return -4;
    }

    //do not allow overlapping mountpoints yet
    if (fs_mount_conflicts(k_target)) {
        return -5;
    }

    object_t *src = handle_get(source);
    if (!src) return -6;

    intptr result = -7;
    if (strcmp(k_type, "fat32") == 0) {
        result = fat32_mount(src, k_target);
    }

    return result;
}

intptr sys_mknode(const char *path, uint32 type) {
    if (!path) return -1;
    char k_path[512];
    if (copy_user_cstr(path, k_path, sizeof(k_path)) != 0) return -1;
    return handle_create(k_path, type);
}

intptr sys_remove(const char *path) {
    if (!path) return -1;
    char k_path[512];
    if (copy_user_cstr(path, k_path, sizeof(k_path)) != 0) return -1;
    return handle_remove(k_path);
}
