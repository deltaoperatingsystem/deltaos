#include <arch/types.h>
#include <arch/io.h>
#include <arch/timer.h>
#include <arch/interrupts.h>
#include <arch/cpu.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <drivers/vt/vt.h>

#define KBD_STATUS      0x64
#define KBD_SC          0x60

//scancode flags
#define SC_RELEASE      0x80
#define SC_SHIFT_L      0x2A
#define SC_SHIFT_R      0x36
#define SC_CTRL         0x1D
#define SC_ALT          0x38

//modifier state
static uint8 mods = 0;

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

//legacy buffer for get_key() compatibility
static char in_codes[256];
static volatile uint8 head = 0;
static volatile uint8 tail = 0;

// keystate bitmap
static bool keystate[128] = {0};

void keyboard_irq(void) {
    uint8 status = inb(KBD_STATUS);
    if (!(status & 1)) return;

    uint8 sc = inb(KBD_SC);
    bool released = (sc & SC_RELEASE) != 0;
    uint8 code = sc & 0x7F;
    
    //update modifiers
    if (code == SC_SHIFT_L || code == SC_SHIFT_R) {
        if (released) mods &= ~VT_MOD_SHIFT;
        else mods |= VT_MOD_SHIFT;
        return;
    }
    if (code == SC_CTRL) {
        if (released) mods &= ~VT_MOD_CTRL;
        else mods |= VT_MOD_CTRL;
        return;
    }
    if (code == SC_ALT) {
        if (released) mods &= ~VT_MOD_ALT;
        else mods |= VT_MOD_ALT;
        return;
    }
    
    // ignore key releases for non-modifiers
    // if (released) return;
    
    //get ASCII (will be extended to codepoint for UTF-8 later)
    char ascii = (mods & VT_MOD_SHIFT) ? scancodes_shift[code] : scancodes_normal[code];
    
    //build VT event
    vt_event_t event = {
        .type = VT_EVENT_KEY,
        .mods = mods,
        .keycode = code,
        .codepoint = (uint32)ascii,  //ASCII is valid unicode codepoint for 0-127
        .pressed = !released
    };
    
    //push to active VT
    vt_t *vt = vt_get_active();
    if (vt) {
        vt_push_event(vt, &event);
    }
    
    //also push to legacy buffer for get_key() compat
    if (ascii) {
        uint8 next = (head + 1) % 256;
        if (next != tail) {
            in_codes[head] = ascii;
            head = next;
        }
    }

    // push to keystate bitmap
    keystate[ascii] = !released;
}

bool get_key(char *c) {
    if (head == tail) return false;
    *c = in_codes[tail];
    tail = (tail + 1) % 256;
    return true;
}

bool get_keystate(char c) {
    return keystate[c];
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