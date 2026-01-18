#include <system.h>
#include <sys/syscall.h>

int readdir(handle_t h, dirent_t *entries, uint32 count, uint32 *index) {
    return (int)__syscall4(SYS_READDIR, h, (long)entries, count, (long)index);
}
