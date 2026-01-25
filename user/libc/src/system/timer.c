#include <system.h>
#include <sys/syscall.h>

uint64 get_ticks(void) {
    return (uint64)__syscall0(SYS_GET_TICKS);
}
