#include <obj/object.h>
#include <mm/kheap.h>
#include <lib/io.h>

object_t *object_create(uint32 type, object_ops_t *ops, void *data) {
    object_t *obj = kmalloc(sizeof(object_t));
    if (!obj) return NULL;
    
    obj->type = type;
    obj->refcount = 1;
    obj->ops = ops;
    obj->data = data;
    
    return obj;
}

void object_ref(object_t *obj) {
    if (!obj) return;
    obj->refcount++;
}

void object_deref(object_t *obj) {
    if (!obj) return;
    if (obj->refcount == 0) {
        printf("[object] ERR: deref on object with refcount 0\n");
        return;
    }
    
    obj->refcount--;
    if (obj->refcount == 0) {
        //call close handler if present
        if (obj->ops && obj->ops->close) {
            obj->ops->close(obj);
        }
        kfree(obj);
    }
}

intptr object_get_info(object_t *obj, uint32 topic, void *buf, size len) {
    if (!obj) return -1;
    
    //generic info handling could go here (e.x type info)
    
    if (obj->ops && obj->ops->get_info) {
        return obj->ops->get_info(obj, topic, buf, len);
    }
    
    return -1;
}

const char *object_get_type_name(object_t *obj) {
    if (!obj) return "null";
    return object_type_name(obj->type);
}
