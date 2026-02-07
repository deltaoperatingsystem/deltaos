#ifndef OBJ_OBJECT_H
#define OBJ_OBJECT_H

#include <arch/types.h>

struct stat;

//base object types
#define OBJECT_NONE    0
#define OBJECT_FILE    1
#define OBJECT_DIR     2
#define OBJECT_DEVICE  3
#define OBJECT_PIPE    4
#define OBJECT_SYSTEM  5

//extended kernel object types
#include <obj/kobject.h>

#define OBJECT_FLAG_NONE      0x00
#define OBJECT_FLAG_ALLOCATED 0x01

struct object;

//polymorphic operations for objects
typedef struct object_ops {
    ssize (*read)(struct object *obj, void *buf, size len, size offset);
    ssize (*write)(struct object *obj, const void *buf, size len, size offset);
    int   (*close)(struct object *obj);  //called when refcount hits 0
    int   (*readdir)(struct object *obj, void *entries, uint32 count, uint32 *index);
    struct object *(*lookup)(struct object *obj, const char *name);  //find child by name
    int   (*stat)(struct object *obj, struct stat *st);
    intptr (*get_info)(struct object *obj, uint32 topic, void *buf, size len);
} object_ops_t;

//base object structure
typedef struct object {
    uint32 type;           //OBJECT_FILE, OBJECT_DIR, OBJECT_PROCESS, etc.
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

//read from object
static inline ssize object_read(object_t *obj, void *buf, size len, size offset) {
    if (!obj || !obj->ops || !obj->ops->read) return -1;
    return obj->ops->read(obj, buf, len, offset);
}

//get type name (for debugging)
const char *object_get_type_name(object_t *obj);

//generic get info
intptr object_get_info(object_t *obj, uint32 topic, void *buf, size len);

#endif

