#include <types.h>

#include <drivers/serial.h>
#include <int/idt.h>
#include <int/pit.h>
#include <int/idt.h>
#include <int/pit.h>

int main() {
    serial_init();
    serial_write("\x1b[2J\x1b[HHello, world!\n");

    idt_init();
    pit_init(100);  //100 Hz = 10ms ticks
    
    serial_write("Timer started waiting for ticks\n");
    
    while (1) {
        __asm__ volatile ("hlt");
    }
    return 0;
}