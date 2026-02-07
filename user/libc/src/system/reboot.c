#include <sys/syscall.h>

void reboot(void) {
    __syscall0(SYS_REBOOT);
}
