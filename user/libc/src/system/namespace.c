#include <sys/syscall.h>
#include <types.h>

//register a handle in the namespace
//allows userspace services to publish their objects for other processes to find
int ns_register(const char *path, int32 h) {
    return __syscall2(SYS_NS_REGISTER, (long)path, (long)h);
}
