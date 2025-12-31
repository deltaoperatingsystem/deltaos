#include <system.h>
#include <io.h>
#include <sys/syscall.h>

int main(int argc, char *argv[]) {
    puts("[user] hello from userspace!\n");

    printf("[user] argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("[user] argv[%d] = %s\n", i, argv[i]);
    }

    int pid = (int)getpid();
    printf("[user] getpid() = %d\n", pid);

    if (pid == 1) {
        puts("attempting spawn()...\n");
        spawn("/initrd/init", 1, (char*[]){"/initrd/init", NULL});
    }
    
    puts("[user] syscall test complete, exiting\n");
    
    return 0;
}
