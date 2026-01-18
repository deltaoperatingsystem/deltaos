#ifndef __SYS_SYSCALL_H
#define __SYS_SYSCALL_H

#define SYS_EXIT            0
#define SYS_GETPID          1
#define SYS_YIELD           2
#define SYS_DEBUG_WRITE     3
#define SYS_SPAWN           4
#define SYS_GET_OBJ         5
#define SYS_HANDLE_READ     6
#define SYS_HANDLE_WRITE    7
#define SYS_HANDLE_SEEK     8

#define SYS_HANDLE_CLOSE    32
#define SYS_HANDLE_DUP      33
#define SYS_CHANNEL_CREATE  34
#define SYS_CHANNEL_SEND    35
#define SYS_CHANNEL_RECV    36
#define SYS_VMO_CREATE      37
#define SYS_VMO_READ        38
#define SYS_VMO_WRITE       39
#define SYS_VMO_MAP         40
#define SYS_VMO_UNMAP       41
#define SYS_NS_REGISTER     42
#define SYS_STAT            43
#define SYS_CHANNEL_TRY_RECV 44
#define SYS_CHANNEL_RECV_MSG 45
#define SYS_CHANNEL_TRY_RECV_MSG 46
#define SYS_WAIT            47

//capability-based process creation (Zircon-style)
#define SYS_PROCESS_CREATE  50  //create suspended process, returns handle
#define SYS_HANDLE_GRANT    51  //inject handle into child process
#define SYS_PROCESS_START   52  //start initial thread in process
#define SYS_VMO_RESIZE      53  //resize a VMO
#define SYS_READDIR         54  //read directory entries

#define SYS_MAX             64

/*
 *System V AMD64 syscall ABI:
 *- syscall number in rax
 *- arguments: rdi, rsi, rdx, r10, r8, r9
 *- return value in rax
 *- rcx and r11 are clobbered by syscall instruction
 */

static inline long __syscall0(long num) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall1(long num, long a1) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall5(long num, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#endif
