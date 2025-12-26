#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <arch/types.h>

word atoi(const char *p);
size strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size n);
char *strchr(const char *s, int c);
char *strtok(char *str, const char *delim);

void *memset(void *s, int c, size n);
void *memcpy(void *dest, const void *src, size n);
void *memmove(void *dest, const void *src, size n);
int memcmp(const void *s1, const void *s2, size n);

#endif