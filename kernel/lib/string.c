#include <arch/types.h>

word atoi(const char *p) {
    word k = 0;
    word sign = 1;
    
    if (*p == '-') {
        sign = -1;
        p++;
    }
    
    while (*p >= '0' && *p <= '9') {
        k = k * 10 + (*p - '0');
        p++;
    }
    return k * sign;
}

size strlen(const char *s) {
    size len = 0;
    while (*s++) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size n) {
    char *d = dest;
    size i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    for (; i < n; i++) d[i] = 0;
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return NULL;
}

char *strtok(char *str, const char *delim) {
    static char *next;
    if (str) next = str;
    if (!next) return NULL;

    char *start = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) {
        *next = '\0';
        next++;
    } else {
        next = NULL;
    }
    return start;
}

void *memset(void *s, int c, size n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}