#include <stdlib.h>
#include <mem.h>
#include <string.h>

int abs(int j) {
    return (j < 0) ? -j : j;
}

int atoi(const char *s) {
    int res = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

double atof(const char *s) {
    double res = 0.0;
    double factor = 1.0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        res = res * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            factor *= 0.1;
            res += (double)(*s - '0') * factor;
            s++;
        }
    }
    return res * sign;
}

char *getenv(const char *name) {
    return NULL;
}

int atexit(void (*function)(void)) {
    return 0; // Stub
}

int system(const char *command) {
    return 0; // Stub
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long acc = 0;
    int any = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;

    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    if ((base == 0 || base == 16) && *s == '0' && (*(s+1) == 'x' || *(s+1) == 'X')) {
        s += 2;
        base = 16;
    }
    if (base == 0) base = 10;

    for (;;) {
        int c = *s;
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'A' && c <= 'Z') c -= 'A' - 10;
        else if (c >= 'a' && c <= 'z') c -= 'a' - 10;
        else break;

        if (c >= base) break;

        acc = acc * base + c;
        any = 1;
        s++;
    }

    if (endptr) *endptr = (char *)(any ? s : nptr);
    return neg ? -acc : acc;
}
