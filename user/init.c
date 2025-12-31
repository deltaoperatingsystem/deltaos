#include <system.h>
#include <io.h>
#include <sys/syscall.h>

void shell(void) {
    int kbd = open("$devices/keyboard", "r");
    while (true) {
        char c;
        if (read(kbd, &c, 1)) {
            putc(c);
        }
    }
}

int main(int argc, char *argv[]) {
    puts("[user] hello from userspace!\n");

    printf("[user] argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("[user] argv[%d] = %s\n", i, argv[i]);
    }

    int pid = (int)getpid();
    printf("[user] getpid() = %d\n", pid);
    
    shell();
    
    return 0;
}
