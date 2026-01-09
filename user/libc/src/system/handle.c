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

int handle_seek(int32 h, size offset, int mode) {
    return __syscall3(SYS_HANDLE_SEEK, (long)h, (long)offset, (long)mode);
}