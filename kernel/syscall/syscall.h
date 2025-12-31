#ifndef SYSCALL_SYSCALL_H
#define SYSCALL_SYSCALL_H

#include <arch/types.h>

//syscall numbers
#define SYS_EXIT            0
#define SYS_GETPID          1
#define SYS_YIELD           2
#define SYS_DEBUG_WRITE     3
#define SYS_WRITE           4   //write to VT console
#define SYS_SPAWN           5   // spawns a new process
#define SYS_OPEN            6   // TEMPORARY!!!
#define SYS_READ            7   // also TEMP

//capability syscalls
#define SYS_HANDLE_CLOSE    32
#define SYS_HANDLE_DUP      33
#define SYS_CHANNEL_CREATE  34
#define SYS_CHANNEL_READ    35
#define SYS_CHANNEL_WRITE   36
#define SYS_VMO_CREATE      37
#define SYS_VMO_READ        38
#define SYS_VMO_WRITE       39

#define SYS_MAX             64

int64 syscall_dispatch(uint64 num, uint64 arg1, uint64 arg2, uint64 arg3,
                       uint64 arg4, uint64 arg5, uint64 arg6);

void syscall_init(void);

#endif
