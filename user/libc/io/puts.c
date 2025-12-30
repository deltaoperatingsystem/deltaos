#include <sys/syscall.h>
#include <string.h>

void puts(const char *str) {
    size len = strlen(str);
    syscall2(SYS_write, (long)str, (long)len);
}
