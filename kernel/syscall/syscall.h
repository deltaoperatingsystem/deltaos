#ifndef SYSCALL_SYSCALL_H
#define SYSCALL_SYSCALL_H

#include <arch/types.h>

//syscall numbers
#define SYS_exit            0
#define SYS_getpid          1
#define SYS_yield           2
#define SYS_debug_write     3
#define SYS_write           4   //write to VT console

//capability syscalls
#define SYS_handle_close    32
#define SYS_handle_dup      33
#define SYS_channel_create  34
#define SYS_channel_read    35
#define SYS_channel_write   36
#define SYS_vmo_create      37
#define SYS_vmo_read        38
#define SYS_vmo_write       39

#define SYS_MAX             64

int64 syscall_dispatch(uint64 num, uint64 arg1, uint64 arg2, uint64 arg3,
                       uint64 arg4, uint64 arg5, uint64 arg6);

void syscall_init(void);

#endif
