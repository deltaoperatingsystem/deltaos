#include <types.h>

size strlen(const char *s) {
    size n = 0;
    while (*s++) n++;
    return n;
}