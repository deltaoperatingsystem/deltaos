#include <arch/types.h>
#include <arch/io.h>
#include <arch/timer.h>
#include <arch/interrupts.h>
#include <arch/cpu.h>
#include <obj/object.h>
#include <obj/namespace.h>

#define KBD_STATUS      0x64
#define KBD_SC          0x60
#define KBD_SHIFT_ON    0x2A
#define KBD_SHIFT_OFF   0xAA

static const char scancodes_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};

static const char scancodes_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};

static char in_codes[256];
static volatile uint8 head = 0;
static volatile uint8 tail = 0;

static bool shift = false;

void keyboard_irq(void) {
    uint8 status = inb(KBD_STATUS);
    if (!(status & 1)) return;

    uint8 sc = inb(KBD_SC);
    if (sc == KBD_SHIFT_ON) { shift = true; return; }
    if (sc == KBD_SHIFT_OFF) { shift = false; return; }
    if (sc > 0x80) return;

    char c = shift ? scancodes_shift[sc] : scancodes_normal[sc];
    if (c) {
        uint8 next = (head + 1) % 256;
        if (next != tail) {
            in_codes[head] = c;
            head = next;
        }
    }
}

bool get_key(char *c) {
    if (head == tail) return false;
    *c = in_codes[tail];
    tail = (tail + 1) % 256;
    return true;
}

void keyboard_wait(void) {
    while (head == tail) arch_halt();
}

//object ops for keyboard - read returns buffered keys
static ssize keyboard_obj_read(object_t *obj, void *buf, size len, size offset) {
    (void)obj;
    (void)offset;
    
    char *out = buf;
    size count = 0;
    while (count < len && head != tail) {
        out[count++] = in_codes[tail];
        tail = (tail + 1) % 256;
    }
    return (ssize)count;  //0 if no keys available
}

static object_ops_t keyboard_object_ops = {
    .read = keyboard_obj_read,
    .write = NULL,
    .close = NULL,
    .ioctl = NULL
};

static object_t *keyboard_object = NULL;

void keyboard_init(void) {
    //flush any pending scancodes
    while (inb(KBD_STATUS) & 1) {
        inb(KBD_SC);
    }
    
    pic_clear_mask(0x1);
    
    keyboard_object = object_create(OBJECT_DEVICE, &keyboard_object_ops, NULL);
    if (keyboard_object) {
        ns_register("$devices/keyboard", keyboard_object);
    }
}