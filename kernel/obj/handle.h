#ifndef OBJ_HANDLE_H
#define OBJ_HANDLE_H

#include <obj/object.h>

//handle is just an index into the handle table
typedef int32 handle_t;

#define INVALID_HANDLE (-1)

//handle table entry
typedef struct {
    object_t *obj;    //the object this handle refers to
    size offset;      //current position (for seekable objects)
    uint32 flags;     //open flags
} handle_entry_t;

//initialize the handle system
void handle_init(void);

//open an object by path (returns handle or INVALID_HANDLE)
handle_t handle_open(const char *path, uint32 flags);

//create a file at path (for filesystems)
int handle_create(const char *path, uint32 type);

//allocate a handle for an object (takes ownership of ref)
handle_t handle_alloc(object_t *obj, uint32 flags);

//get object from handle (does NOT add ref)
object_t *handle_get(handle_t h);

//read from handle
ssize handle_read(handle_t h, void *buf, size len);

//write to handle
ssize handle_write(handle_t h, const void *buf, size len);

//seek
ssize handle_seek(handle_t h, ssize offset, int whence);

//close handle (releases object ref)
int handle_close(handle_t h);

//read directory entries
int handle_readdir(handle_t h, void *entries, uint32 count);

//seek whence values
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif
