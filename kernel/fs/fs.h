#ifndef FS_FS_H
#define FS_FS_H

#include <arch/types.h>
#include <obj/object.h>

//filesystem node types
#define FS_TYPE_FILE    1   //regular file
#define FS_TYPE_DIR     2   //directory
#define FS_TYPE_SYMLINK 3   //symbolic link
#define FS_TYPE_LINK    4   //hard link
#define FS_TYPE_PIPE    5   //named pipe (FIFO)
#define FS_TYPE_SOCKET  6   //socket
#define FS_TYPE_DEVICE  7   //device node

//directory entry (name points to fs-managed storage)
typedef struct dirent {
    const char *name;   //entry name (managed by filesystem)
    uint32 type;        //FS_TYPE_*
} dirent_t;

struct fs;

//filesystem operations
typedef struct fs_ops {
    //lookup path within this filesystem returns object with +1 ref
    object_t *(*lookup)(struct fs *fs, const char *path);
    
    //create a new file/dir at path
    int (*create)(struct fs *fs, const char *path, uint32 type);
    
    //remove file/dir at path
    int (*remove)(struct fs *fs, const char *path);
    
    //read directory entries (index is in/out for stateless iteration)
    int (*readdir)(struct fs *fs, const char *path, dirent_t *entries, uint32 count, uint32 *index);
} fs_ops_t;

//filesystem instance
typedef struct fs {
    const char *name;
    fs_ops_t *ops;
    void *data;  //fs-specific data
} fs_t;

#endif
