#include <sys/syscall.h>
#include <stdio.h>

int main() {
    printf("Rebooting...\n");
    reboot();
    return 0;
}
