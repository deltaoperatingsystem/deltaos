#ifndef _LIBC_STRING_H
#define _LIBC_STRING_H

#include <types.h>

size strlen(const char *s);
bool streq(const char *a, const char *b);
char *strchr(const char *s, int c);
char *strtok(char *str, const char *delim);

void *memcpy(void *dest, const void *src, size len);
void *memset(void *s, int c, size len);
void *memmove(void *dest, const void *src, size len);
int memcmp(const void *s1, const void *s2, size len);

#endif