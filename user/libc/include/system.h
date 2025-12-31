#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <types.h>
#include <sys/syscall.h>

void exit(int code);
int64 getpid(void);
void yield(void);
int spawn(char *path, int argc, char **argv);
int open(const char *path, const char *perms);
int read(int handle, void *buffer, int n);

#endif