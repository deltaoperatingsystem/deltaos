#include <sys/syscall.h>
#include <types.h>

//register a handle in the namespace with a rights ceiling applied to any caller that opens it
//allows userspace services to publish their objects for other processes to find
int ns_register(const char *path, int32 h, uint32 max_rights) {
    return __syscall3(SYS_NS_REGISTER, (long)path, (long)h, (long)max_rights);
}
