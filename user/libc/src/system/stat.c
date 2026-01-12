#include <system.h>
#include <sys/syscall.h>

int stat(const char *path, stat_t *st) {
    return __syscall2(SYS_STAT, (long)path, (long)st);
}
