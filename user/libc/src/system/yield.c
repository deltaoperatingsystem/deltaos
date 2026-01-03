#include <sys/syscall.h>

void yield(void) {
    __syscall0(SYS_YIELD);
}
