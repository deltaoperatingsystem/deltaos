#include <types.h>

#include <drivers/serial.h>
#include <int/idt.h>
#include <int/pit.h>
#include <drivers/keyboard.h>

int main() {
    serial_init();
    serial_write("\x1b[2J\x1b[HHello, world!\n");

    idt_init();
    pit_init(1000);
    keyboard_init();

    // keep kernel running so interrupts continue to be delivered
    while (1) {
        char *s;
        if (get_key(s)) serial_write_char(*s);
        __asm__ volatile ("hlt");
    }
}