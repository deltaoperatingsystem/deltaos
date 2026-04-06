#ifndef FS_MOUNT_H
#define FS_MOUNT_H

#include <fs/fs.h>

//register a filesystem at an absolute mountpoint path
int fs_mount_register(const char *target, fs_t *fs);

//resolve an absolute path against the mount table
//returns 1 when a mount matches 0 otherwise
int fs_mount_resolve(const char *path, fs_t **fs_out, const char **fs_path_out);

//check whether a mountpoint would overlap an existing mount
int fs_mount_conflicts(const char *target);

#endif
