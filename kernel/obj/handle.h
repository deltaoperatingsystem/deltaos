#ifndef OBJ_HANDLE_H
#define OBJ_HANDLE_H

#include <obj/object.h>
#include <obj/rights.h>

//forward declare (defined in fs/fs.h)
struct stat;
typedef struct stat stat_t;

//handle is just an index into a process's handle table
typedef int32 handle_t;

#define INVALID_HANDLE (-1)

//initialize the handle/namespace system
void handle_init(void);

//open an object by path with rights (uses current process)
handle_t handle_open(const char *path, handle_rights_t rights);

//create a file at path (for filesystems)
int handle_create(const char *path, uint32 type);

//allocate a handle for an object with rights (current process)
handle_t handle_alloc(object_t *obj, handle_rights_t rights);

//get object from handle (does NOT add ref)
object_t *handle_get(handle_t h);

//check if handle has required rights
int handle_has_rights(handle_t h, handle_rights_t required);

//duplicate a handle with reduced rights
handle_t handle_duplicate(handle_t h, handle_rights_t new_rights);

//read from handle (requires HANDLE_RIGHT_READ)
ssize handle_read(handle_t h, void *buf, size len);

//write to handle (requires HANDLE_RIGHT_WRITE)
ssize handle_write(handle_t h, const void *buf, size len);

//seek
ssize handle_seek(handle_t h, ssize offset, int whence);

//close handle (releases object ref)
int handle_close(handle_t h);

//read directory entries
int handle_readdir(handle_t h, void *entries, uint32 count);

//stat a path (without opening)
int handle_stat(const char *path, stat_t *st);

//stat a handle
int handle_fstat(handle_t h, stat_t *st);

//remove a path
int handle_remove(const char *path);

//seek whence values
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif

