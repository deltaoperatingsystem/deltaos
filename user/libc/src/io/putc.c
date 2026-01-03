#include <io.h>
#include <system.h>

void putc(const char c) {
    if (__stdout == INVALID_HANDLE) return;
    handle_write(__stdout, &c, 1);
}