#include <io.h>
#include <system.h>
#include <string.h>

void puts(const char *str) {
    if (__stdout == INVALID_HANDLE) return;
    size len = strlen(str);
    handle_write(__stdout, str, len);
}
