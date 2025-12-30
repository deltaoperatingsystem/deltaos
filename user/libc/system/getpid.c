#include <types.h>
#include <sys/syscall.h>

int64 getpid(void) {
    return syscall0(SYS_getpid);
}