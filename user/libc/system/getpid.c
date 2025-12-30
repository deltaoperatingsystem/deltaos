#include <types.h>
#include <system.h>

// __asm__ volatile("mov $1, %%rax\n\tsyscall\n\t" ::: "rax","rcx","r11","memory");

int64 getpid() {
    return __syscall(1, 0, 0, 0, 0, 0, 0);
}