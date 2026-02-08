#ifndef __STDLIB_H
#define __STDLIB_H

#include <types.h>
#include <system.h>
#include <mem.h>

int abs(int j);
int atoi(const char *nptr);
double atof(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);


void exit(int status);
int atexit(void (*function)(void));

char *getenv(const char *name);
int system(const char *command);

#endif
