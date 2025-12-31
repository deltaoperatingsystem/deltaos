#include <sys/syscall.h>

int open(const char *path, const char *perms) {
    return __syscall2(SYS_OPEN, (long)path, (long)perms);
}