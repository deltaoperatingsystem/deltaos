#include <arch/types.h>
#include <arch/io.h>
#include <obj/object.h>
#include <obj/namespace.h>

#define COM1_PORT 0x3F8
#define COM1_MMIO 0

void serial_init(void) {
    if (ARCH_HAS_PORT_IO) {
        outb(COM1_PORT + 1, 0x00);
        outb(COM1_PORT + 3, 0x80);
        outb(COM1_PORT + 0, 0x03);
        outb(COM1_PORT + 1, 0x00);
        outb(COM1_PORT + 3, 0x03);
        outb(COM1_PORT + 2, 0xC7);
        outb(COM1_PORT + 4, 0x0B);
    }
}

static uint8 serial_is_transmit_empty(void) {
    if (ARCH_HAS_PORT_IO) {
        return inb(COM1_PORT + 5) & 0x20;
    } else {
        return arch_mmio_read8(COM1_MMIO + 5) & 0x20;
    }
}

void serial_write_char(char c) {
    while (!serial_is_transmit_empty());
    if (ARCH_HAS_PORT_IO) {
        outb(COM1_PORT, c);
    } else {
        arch_mmio_write8(COM1_MMIO, c);
    }
}

void serial_write(const char* s) {
    while (*s) {
        if (*s == '\n')
            serial_write_char('\r');
        serial_write_char(*s++);
    }
}

void serial_write_hex(uint64 n) {
    char const* hex = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        serial_write_char(hex[(n >> i) & 0xF]);
    }
}

//object ops for serial - write to serial port
static ssize serial_obj_write(object_t *obj, const void *buf, size len, size offset) {
    (void)obj;
    (void)offset;
    const char *s = buf;
    for (size i = 0; i < len; i++) {
        if (s[i] == '\n') serial_write_char('\r');
        serial_write_char(s[i]);
    }
    return len;
}

static object_ops_t serial_object_ops = {
    .read = NULL,
    .write = serial_obj_write,
    .close = NULL,
    .ioctl = NULL
};

static object_t *serial_object = NULL;

void serial_init_object(void) {
    serial_object = object_create(OBJECT_DEVICE, &serial_object_ops, NULL);
    if (serial_object) {
        ns_register("$devices/serial", serial_object);
    }
}
