#include <stdio.h>

int fgetc(FILE* f) {
    if (f->handle == INVALID_HANDLE) return -1;
    unsigned char c;
    int code;
    if ((code = handle_read(f->handle, &c, 1)) < 0) {
        return code;
    }
    if (code == 0) return -1;
    return c;
}
