#include <system.h>
#include <sys/syscall.h>

int ping(uint8 a, uint8 b, uint8 c, uint8 d, uint32 count) {
    return __syscall5(SYS_PING, (long)a, (long)b, (long)c, (long)d, (long)count);
}
