#include <sys/syscall.h>
#include <types.h>

//get object handle from namespace
//parent: parent handle or INVALID_HANDLE (-1) for root namespace
//path:   path within namespace
//rights: requested rights
int32 get_obj(int32 parent, const char *path, uint32 rights) {
    return __syscall3(SYS_GET_OBJ, (long)parent, (long)path, (long)rights);
}

//read from handle
int handle_read(int32 h, void *buf, int len) {
    return __syscall3(SYS_HANDLE_READ, (long)h, (long)buf, (long)len);
}

//write to handle
int handle_write(int32 h, const void *buf, int len) {
    return __syscall3(SYS_HANDLE_WRITE, (long)h, (long)buf, (long)len);
}

//seek handle
int handle_seek(int32 h, size offset, int mode) {
    return __syscall3(SYS_HANDLE_SEEK, (long)h, (long)offset, (long)mode);
}

//close handle
int handle_close(int32 h) {
    return __syscall1(SYS_HANDLE_CLOSE, (long)h);
}

//duplicate handle with same or reduced rights
int32 handle_dup(int32 h, uint32 new_rights) {
    return __syscall2(SYS_HANDLE_DUP, (long)h, (long)new_rights);
}

int object_get_info(int32 h, uint32 topic, void *ptr, uint64 len) {
    return __syscall4(SYS_OBJECT_GET_INFO, (long)h, (long)topic, (long)ptr, (long)len);
}
