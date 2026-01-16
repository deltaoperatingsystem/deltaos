#include <system.h>
#include <sys/syscall.h>

int wait(int pid) {
    return (int)__syscall1(SYS_WAIT, (uint64)pid);
}
