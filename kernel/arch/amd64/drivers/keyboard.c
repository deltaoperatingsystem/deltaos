#include <types.h>
#include <io/port.h>
#include <int/pic.h>

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

void keyboard_init(void) {
    pic_clear_mask(0x1);
}