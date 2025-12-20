#include <types.h>

#include <drivers/serial.h>
#include <arch/amd64/int/idt.h>

int main() {
    serial_init();
    serial_write("\x1b[2J\x1b[HHello, world!\n");

    idt_init();
    serial_write("IDT initialized triggering int $3 t test\n");
    __asm__ volatile ("int $3");
    serial_write("returned from interrupt\n");
    
    return 0;
}