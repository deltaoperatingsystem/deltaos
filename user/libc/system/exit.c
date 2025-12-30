#include <sys/syscall.h>

__attribute__((noreturn)) void exit(int code) {
    syscall1(SYS_exit, code);
    __builtin_unreachable();
}