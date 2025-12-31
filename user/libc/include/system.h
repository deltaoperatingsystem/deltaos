#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <types.h>
#include <sys/syscall.h>

void exit(int code);
int64 getpid(void);
void yield(void);
int spawn(char *path, int argc, char **argv);

#endif