#include <syscall/syscall.h>
#include <fs/fs.h>
#include <proc/process.h>
#include <lib/string.h>
#include <lib/path.h>
#include <lib/io.h>

intptr sys_handle_read(handle_t h, void *buf, size len) {
    if (!buf || len == 0) return -1;
    return handle_read(h, buf, len);
}

intptr sys_handle_write(handle_t h, const void *buf, size len) {
    if (!buf || len == 0) return -1;
    return handle_write(h, buf, len);
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

//TODO: allow for directory creation in non-$files namespaces
intptr sys_mkdir(const char *path, uint32 mode) {
    (void)mode;
    if (!path) return -1;

    process_t *proc = process_current();
    if (!proc) return -2;

    if (path[0] == '$') return -4;

    char full_path[256];
    if (path[0] == '/') {
        strncpy(full_path, path, sizeof(full_path) - 1);
    } else {
        snprintf(full_path, sizeof(full_path), "$files/%s/%s", proc->cwd, path);
    }

    path_normalize(full_path);

    return handle_create(full_path, FS_TYPE_DIR);
}

intptr sys_remove(const char *path) {
    if (!path) return -1;
    return handle_remove(path);
}
