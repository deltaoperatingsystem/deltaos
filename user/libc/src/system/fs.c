#include <system.h>
#include <sys/syscall.h>

int mkdir(const char *path) {
    return __syscall2(SYS_MKNODE, (long)path, (long)FS_TYPE_DIR);
}

int mkfile(const char *path) {
    return __syscall2(SYS_MKNODE, (long)path, (long)FS_TYPE_FILE);
}

int mount(int32 source, const char *target, const char *fstype) {
    if (!fstype) fstype = "fat32";
    return __syscall3(SYS_MOUNT, (long)source, (long)target, (long)fstype);
}
