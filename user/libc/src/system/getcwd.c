#include <system.h>
#include <sys/syscall.h>

int getcwd(char *buf, size size) {
    return (int)__syscall2(SYS_GETCWD, (long)buf, size);
}
