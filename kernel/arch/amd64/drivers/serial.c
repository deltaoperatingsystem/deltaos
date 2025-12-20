#include <types.h>
#include <io/port.h>

#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8
#define COM5 0x5F8
#define COM6 0x4F8
#define COM7 0x5E8
#define COM8 0x4E8

void serial_init() {
    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB
    outb(COM1 + 0, 0x03);    // Baud divisor low byte (38400 baud)
    outb(COM1 + 1, 0x00);    // Baud divisor high byte
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear them, 14‑byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}


uint8 serial_is_transmit_empty() {
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    while (!serial_is_transmit_empty());
    outb(COM1, c);
}

void serial_write(const char* s) {
    while (*s) {
        if (*s == '\n')
            serial_write_char('\r');
        serial_write_char(*s++);
    }
}