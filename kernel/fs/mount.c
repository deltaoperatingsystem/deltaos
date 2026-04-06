#include <fs/mount.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <lib/io.h>

#define MOUNT_PATH_MAX 256

typedef struct fs_mount_entry {
    //mount target path in place
    char target[MOUNT_PATH_MAX];
    fs_t *fs;
    struct fs_mount_entry *next;
} fs_mount_entry_t;

static fs_mount_entry_t *mounts = NULL;
static spinlock_t mount_lock = SPINLOCK_INIT;

static size mount_path_len(const char *path) {
    if (!path) return 0;
    return strlen(path);
}

static int mount_path_matches(const char *target, const char *path) {
    size target_len = mount_path_len(target);
    if (!target_len || !path) return 0;

    //root matches everything that starts with /
    if (target_len == 1 && target[0] == '/') {
        return path[0] == '/';
    }

    if (strncmp(path, target, target_len) != 0) return 0;
    if (path[target_len] == '\0' || path[target_len] == '/') return 1;
    return 0;
}

static int mount_paths_overlap(const char *a, const char *b) {
    size a_len = mount_path_len(a);
    size b_len = mount_path_len(b);

    if (!a_len || !b_len) return 0;
    if (a_len == 1 && a[0] == '/') return 1;
    if (b_len == 1 && b[0] == '/') return 1;

    if (strncmp(a, b, a_len < b_len ? a_len : b_len) == 0) {
        //shared prefix has to end on a path boundary
        if (a_len == b_len) return 1;
        if (a_len < b_len) return (b[a_len] == '\0' || b[a_len] == '/');
        return (a[b_len] == '\0' || a[b_len] == '/');
    }
    return 0;
}

int fs_mount_conflicts(const char *target) {
    if (!target || target[0] != '/') return 1;

    //keep mount points from overlapping or nesting
    spinlock_acquire(&mount_lock);
    for (fs_mount_entry_t *e = mounts; e; e = e->next) {
        if (mount_paths_overlap(e->target, target) || mount_paths_overlap(target, e->target)) {
            spinlock_release(&mount_lock);
            return 1;
        }
    }
    spinlock_release(&mount_lock);
    return 0;
}

int fs_mount_register(const char *target, fs_t *fs) {
    if (!target || !fs || target[0] != '/') return -1;
    if (fs_mount_conflicts(target)) return -1;

    fs_mount_entry_t *entry = kzalloc(sizeof(fs_mount_entry_t));
    if (!entry) return -1;

    //reject mount paths that do not fit in the fixed buffer
    size len = strlen(target);
    if (len >= sizeof(entry->target)) {
        kfree(entry);
        return -1;
    }

    memcpy(entry->target, target, len + 1);
    entry->fs = fs;

    spinlock_acquire(&mount_lock);
    entry->next = mounts;
    mounts = entry;
    spinlock_release(&mount_lock);

    printf("[mount] mounted %s at %s\n", fs->name ? fs->name : "fs", target);
    return 0;
}

int fs_mount_resolve(const char *path, fs_t **fs_out, const char **fs_path_out) {
    if (!path || path[0] != '/') return 0;

    fs_mount_entry_t *best = NULL;
    size best_len = 0;

    //pick the most specific mount that matches the path
    spinlock_acquire(&mount_lock);
    for (fs_mount_entry_t *e = mounts; e; e = e->next) {
        if (!mount_path_matches(e->target, path)) continue;
        size len = strlen(e->target);
        //deepest mount wins
        if (!best || len > best_len) {
            best = e;
            best_len = len;
        }
    }
    spinlock_release(&mount_lock);

    if (!best) return 0;

    if (fs_out) *fs_out = best->fs;
    if (fs_path_out) {
        //return the path relative to the mounted fs
        if (path[best_len] == '\0') {
            *fs_path_out = "";
        } else if (path[best_len] == '/') {
            *fs_path_out = path + best_len + 1;
        } else {
            *fs_path_out = path + best_len;
        }
    }

    return 1;
}
