#include <sys/syscall.h>

int read(int handle, void *buffer, int n) {
    return __syscall3(SYS_READ, (long)handle, (long)buffer, (long)n);
}