#include <sys/syscall.h>

void shutdown(void) {
    __syscall0(SYS_SHUTDOWN);
}
