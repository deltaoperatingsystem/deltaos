#include <system.h>
#include <sys/syscall.h>

int chdir(const char *path) {
    return (int)__syscall1(SYS_CHDIR, (long)path);
}
