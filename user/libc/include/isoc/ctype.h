#ifndef _CTYPE_H
#define _CTYPE_H

static inline int isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r');
}

static inline int isprint(int c) {
    return (c >= 0x20 && c <= 0x7E);
}

static inline int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

static inline int isupper(int c) {
    return (c >= 'A' && c <= 'Z');
}

static inline int islower(int c) {
    return (c >= 'a' && c <= 'z');
}

static inline int isalpha(int c) {
    return isupper(c) || islower(c);
}

static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

static inline int toupper(int c) {
    if (islower(c)) return c - 32;
    return c;
}

static inline int tolower(int c) {
    if (isupper(c)) return c + 32;
    return c;
}

#endif
