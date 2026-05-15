#ifndef _STRING_H
#define _STRING_H

#include <stdint.h>
#include <stddef.h>

int is_whitespace(char c);
int is_digit(char c);
int str_starts_with(const char *str, const char *prefix);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

#endif
