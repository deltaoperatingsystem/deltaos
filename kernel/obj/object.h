#ifndef OBJ_OBJECT_H
#define OBJ_OBJECT_H

#include <arch/types.h>

//object types
#define OBJECT_NONE    0
#define OBJECT_FILE    1
#define OBJECT_DIR     2
#define OBJECT_DEVICE  3
#define OBJECT_PIPE    4

struct object;

//polymorphic operations for objects
typedef struct object_ops {
    ssize (*read)(struct object *obj, void *buf, size len, size offset);
    ssize (*write)(struct object *obj, const void *buf, size len, size offset);
    int   (*close)(struct object *obj);  //called when refcount hits 0
    int   (*ioctl)(struct object *obj, uint32 cmd, void *arg);
    int   (*readdir)(struct object *obj, void *entries, uint32 count, uint32 *index);
} object_ops_t;

//base object structure
typedef struct object {
    uint32 type;           //OBJECT_FILE, OBJECT_DIR, etc.
    uint32 refcount;       //freed when 0
    object_ops_t *ops;     //polymorphic operations
    void *data;            //type-specific data
} object_t;

//create a new object
object_t *object_create(uint32 type, object_ops_t *ops, void *data);

//increment reference count
void object_ref(object_t *obj);

//decrement reference count (frees if 0)
void object_deref(object_t *obj);

//alias for object_deref
static inline void object_release(object_t *obj) { object_deref(obj); }

//write to object
static inline ssize object_write(object_t *obj, const void *buf, size len, size offset) {
    if (!obj || !obj->ops || !obj->ops->write) return -1;
    return obj->ops->write(obj, buf, len, offset);
}

#endif
