#include <sys/syscall.h>
#include <system.h>
#include <string.h>
#include <io.h>

#include <math.h>
void shell(void) {
    int kbd = open("$devices/keyboard", "r");
    char buffer[128];
    size l = 0;
    while (true) {
        char c;
        if (read(kbd, &c, 1)) {
            buffer[l++] = c;
            putc(c);
            if (c == '\n') {
                buffer[l] = '\0';
                char *cmd = strtok(buffer, " \t");
                if (streq(cmd, "help")) {
                    puts("HELP MEEEE!!!\n");
                } else if (streq(cmd, "test")) {
                    printf("%f", atan(1.0));
                }
                l = 0;
            }
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
