#include <system.h>
#include <sys/syscall.h>

int32 process_create(const char *name) {
    return (int32)__syscall1(SYS_PROCESS_CREATE, (long)name);
}

int handle_grant(int32 proc_h, int32 local_h, uint32 rights) {
    return (int)__syscall3(SYS_HANDLE_GRANT, (long)proc_h, (long)local_h, (long)rights);
}

int process_start(int32 proc_h, uint64 entry, uint64 stack) {
    return (int)__syscall3(SYS_PROCESS_START, (long)proc_h, (long)entry, (long)stack);
}
