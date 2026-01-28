#include <system.h>
#include <sys/syscall.h>

int stat(const char *path, stat_t *st) {
    return __syscall2(SYS_STAT, (long)path, (long)st);
}

int fstat(handle_t h, stat_t *st) {
    return __syscall2(SYS_FSTAT, (long)h, (long)st);
}

int mkdir(const char *path, uint32 mode) {
    return __syscall2(SYS_MKDIR, (long)path, (long)mode);
}
