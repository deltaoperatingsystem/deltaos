#include <system.h>

// __asm__ volatile(
//     "mov $0, %%rdi\n\t"
//     "mov $0, %%rax\n\t"
//     "syscall\n\t"
//     :
//     :
//     : "rax","rdi","rcx","r11","memory"
// );

void exit(int code) {
    __syscall(0, code, 0, 0, 0, 0, 0);
}