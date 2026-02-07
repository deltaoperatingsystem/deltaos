#include <sys/syscall.h>
#include <stdio.h>

int main() {
    printf("Shutting down...\n");
    shutdown();
    return 0;
}
