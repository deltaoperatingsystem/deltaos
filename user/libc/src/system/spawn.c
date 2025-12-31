#include <sys/syscall.h>

// __syscall3(SYS_SPAWN, (long)"/initrd/init", 1, (long)(char*[]){"/initrd/init", NULL});

int spawn(char *path, int argc, char **argv) {
    __syscall3(SYS_SPAWN, (long)path, argc, (long)argv);
}