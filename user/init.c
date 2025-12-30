#include <system.h>
#include <io.h>

__attribute__((noreturn)) void _start(void) {
    /* write hello */
    puts("[user] hello from userspace!\n");

    /* getpid (result ignored) */
    getpid();

    /* write done */
    puts("[user] syscall test complete, exiting\n");

    /* exit(0) */
    exit(0);

    for(;;) __asm__ volatile("hlt");
}
