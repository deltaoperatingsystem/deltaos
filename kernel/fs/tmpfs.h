#ifndef FS_TMPFS_H
#define FS_TMPFS_H

#include <fs/fs.h>

//initialize tmpfs and register at $files
void tmpfs_init(void);

//create a file in tmpfs (returns 0 on success)
int tmpfs_create(const char *path);

//create a directory in tmpfs (returns 0 on success)
int tmpfs_create_dir(const char *path);

//open a file from tmpfs (returns object with +1 ref)
object_t *tmpfs_open(const char *path);

#endif
