#include <syscall/syscall.h>
#include <arch/power.h>

intptr sys_reboot(void) {
    arch_power_reboot();
    return 0;
}

intptr sys_shutdown(void) {
    arch_power_shutdown();
    return 0;
}
