#include <string.h>

#include <types.h>
#include <sys/syscall.h>

void *memcpy(void *dest, const void *src, size n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    for (size i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *memset(void *s, int c, size n) {
    unsigned char *p = s;
    for (size i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

void *memmove(void *dest, const void *src, size n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        for (size i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size n) {
    const unsigned char *p1 = s1;
    const unsigned char *p2 = s2;
    for (size i = 0; i < n; i++) {
        if (p1[i] < p2[i]) return -1;
        if (p1[i] > p2[i]) return 1;
    }
    return 0;
}
