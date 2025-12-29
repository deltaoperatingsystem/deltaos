#ifndef OBJ_RIGHTS_H
#define OBJ_RIGHTS_H

#include <arch/types.h>

/*
 *handle rights
 * 
 *rights control what operations can be performed on a handle.
 *when duplicating a handle rights can only be reduced never expanded
 *when transferring a handle via channel rights are preserved
 */

//individual rights
#define HANDLE_RIGHT_NONE       0
#define HANDLE_RIGHT_DUPLICATE  (1 << 0) //can call handle_duplicate
#define HANDLE_RIGHT_TRANSFER   (1 << 1) //can be sent via channel
#define HANDLE_RIGHT_READ       (1 << 2) //can read from object
#define HANDLE_RIGHT_WRITE      (1 << 3) //can write to object
#define HANDLE_RIGHT_EXECUTE    (1 << 4) //can execute/map executable
#define HANDLE_RIGHT_MAP        (1 << 5) //can map to address space (VMO)
#define HANDLE_RIGHT_GET_INFO   (1 << 6) //can query object info
#define HANDLE_RIGHT_SIGNAL     (1 << 7) //can signal/wait on object
#define HANDLE_RIGHT_DESTROY    (1 << 8) //can destroy the object (process/thread)

//convenience combinations
#define HANDLE_RIGHTS_BASIC     (HANDLE_RIGHT_DUPLICATE | HANDLE_RIGHT_TRANSFER)
#define HANDLE_RIGHTS_IO        (HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE)
#define HANDLE_RIGHTS_DEFAULT   (HANDLE_RIGHTS_BASIC | HANDLE_RIGHTS_IO | HANDLE_RIGHT_GET_INFO)
#define HANDLE_RIGHTS_ALL       0xFFFFFFFF

//type alias for rights bitmask
typedef uint32 handle_rights_t;

//check if rights contain required rights
static inline int rights_has(handle_rights_t rights, handle_rights_t required) {
    return (rights & required) == required;
}

//reduce rights (returns intersection)
static inline handle_rights_t rights_reduce(handle_rights_t original, handle_rights_t mask) {
    return original & mask;
}

#endif
