#ifndef _LIBC_STRING_H
#define _LIBC_STRING_H

#include <types.h>

size strlen(const char *s);
bool streq(const char *a, const char *b);
int strcmp(const char *s1, const char *s2);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strtok(char *str, const char *delim);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size n);
char *strdup(const char *s);
char *strcat(char *dest, const char *src);
char *strstr(const char *haystack, const char *needle);
int strncmp(const char *s1, const char *s2, size n);

void *memcpy(void *dest, const void *src, size len);
void *memset(void *s, int c, size len);
void *memmove(void *dest, const void *src, size len);
int memcmp(const void *s1, const void *s2, size len);

#endif