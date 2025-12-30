#include <system.h>
#include <string.h>

void puts(const char *str) {
    unsigned long len = (unsigned long)strlen(str);
    __syscall(4, (long)str, len, 0, 0, 0, 0);
}
