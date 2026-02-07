#include <arch/amd64/power.h>
#include <lib/io.h>
#include <arch/amd64/acpi/acpi.h>

void arch_power_reboot(void) {
    acpi_reboot();
}

void arch_power_shutdown(void) {
    acpi_shutdown();
}
