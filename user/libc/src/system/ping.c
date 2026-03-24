#include <system.h>
#include <sys/syscall.h>

int ping(uint8 family, const void *addr, uint32 addr_len, uint32 count) {
    return (int)__syscall4(SYS_PING, (long)family, (long)addr, (long)addr_len, (long)count);
}
