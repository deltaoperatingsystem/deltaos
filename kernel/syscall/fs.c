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

static __attribute__((noinline)) int write_user_byte(uint8 *ptr, uint8 value) {
    percpu_t *cpu = percpu_get();
    cpu->recovery_rip = (uintptr)&&fault;
    atomic_signal_fence(memory_order_seq_cst);
    *ptr = value;
    atomic_signal_fence(memory_order_seq_cst);
    cpu->recovery_rip = 0;
    return 0;
fault:
    cpu->recovery_rip = 0;
    return -EFAULT;
}

#define MAX_HANDLE_IO (1u << 20)

static int copy_to_user_bytes(void *user_ptr, const void *kernel_buf, size len) {
    if (len == 0) return 0;
    if (!user_ptr || !kernel_buf) return -EFAULT;

    uintptr dst_addr = (uintptr)user_ptr;
    if (dst_addr < USER_SPACE_START || dst_addr >= USER_SPACE_END) return -EFAULT;
    if (len > (size)(USER_SPACE_END - dst_addr)) return -EFAULT;

    const uint8 *src = (const uint8 *)kernel_buf;
    uint8 *dst = (uint8 *)user_ptr;
    size i = 0;

    while (i < len && ((uintptr)&dst[i] & (sizeof(uintptr) - 1))) {
        if (write_user_byte(&dst[i], src[i]) != 0) return -EFAULT;
        i++;
    }

    if (i + sizeof(uintptr) <= len) {
        percpu_t *cpu = percpu_get();
        cpu->recovery_rip = (uintptr)&&bulk_fault;

        while (i + sizeof(uintptr) <= len) {
            uintptr tmp = 0;
            memcpy(&tmp, &src[i], sizeof(tmp));
            *(uintptr *)&dst[i] = tmp;
            i += sizeof(uintptr);
        }

        cpu->recovery_rip = 0;
    }

    while (i < len) {
        if (write_user_byte(&dst[i], src[i]) != 0) return -EFAULT;
        i++;
    }

    return 0;

bulk_fault:
    percpu_get()->recovery_rip = 0;
    return -EFAULT;
}

intptr sys_handle_read(handle_t h, void *buf, size len) {
    if (!buf || len == 0) return -1;
    if (len > MAX_HANDLE_IO) return -1;
    void *kbuf = kmalloc(len);
    if (!kbuf) return -1;

    ssize result = handle_read(h, kbuf, len);
    if (result > 0) {
        if (copy_to_user_bytes(buf, kbuf, (size)result) != 0) {
            kfree(kbuf);
            return -1;
        }
    }

    kfree(kbuf);
    return result;
}

intptr sys_handle_write(handle_t h, const void *buf, size len) {
    if (!buf || len == 0) return -1;
    if (len > MAX_HANDLE_IO) return -1;
    void *kbuf = kmalloc(len);
    if (!kbuf) return -1;

    if (copy_user_bytes(buf, kbuf, len) != 0) {
        kfree(kbuf);
        return -1;
    }

    intptr result = handle_write(h, kbuf, len);
    kfree(kbuf);
    return result;
}

intptr sys_handle_seek(handle_t h, size offset, int mode) {
    return handle_seek(h, offset, mode);
}

intptr sys_stat(const char *path, stat_t *st) {
    return handle_stat(path, st);
}

intptr sys_fstat(handle_t h, stat_t *st) {
    return handle_fstat(h, st);
}

intptr sys_readdir(handle_t h, dirent_t *entries, uint32 count, uint32 *index) {
    if (!entries || !index || count == 0) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    object_t *obj = process_get_handle(proc, h);
    if (!obj) return -2;
    
    if (!obj->ops || !obj->ops->readdir) return -3;
    
    return obj->ops->readdir(obj, entries, count, index);
}

intptr sys_chdir(const char *path) {
    if (!path) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    if (path[0] == '$') return -4;
    
    stat_t st;
    if (handle_stat(path, &st) != 0) return -2;
    if (st.type != FS_TYPE_DIR) return -3;
    
    char full_path[256];
    if (path[0] == '/') {
        strncpy(full_path, path, sizeof(full_path) - 1);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", proc->cwd, path);
    }
    
    path_normalize(full_path);
    
    size final_len = strlen(full_path);
    if (final_len >= sizeof(proc->cwd)) return -1;
    
    memcpy(proc->cwd, full_path, final_len + 1);
    return 0;
}

intptr sys_getcwd(char *buf, size bufsize) {
    if (!buf || bufsize == 0) return -1;
    
    process_t *proc = process_current();
    if (!proc) return -1;
    
    size cwd_len = strlen(proc->cwd);
    if (cwd_len >= bufsize) return -1;
    
    memcpy(buf, proc->cwd, cwd_len + 1);
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
