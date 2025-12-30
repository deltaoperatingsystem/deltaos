#ifndef __SYSTEM_H
#define __SYSTEM_H

#include <types.h>

long __syscall(long num, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6);
void exit(int code);
int64 getpid();

#endif