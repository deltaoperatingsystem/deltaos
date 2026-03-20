#include <types.h>
#include <stdio.h>

char* fgets(char* buf, int bufsz, FILE* f) {
    if (bufsz <= 0) {
        return NULL;
    }
    
    int c;
    size_t pos = 0;
    
    while ((c = fgetc(f)) != -1) {
        if (pos >= (size_t)(bufsz - 1)) {
            break;
        }
        buf[pos++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    
    if (pos == 0 && c == -1) {
        return NULL;
    }
    
    buf[pos] = '\0';
    return buf;
}
