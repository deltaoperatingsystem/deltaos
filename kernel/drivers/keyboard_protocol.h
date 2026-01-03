#ifndef DRIVERS_KEYBOARD_PROTOCOL_H
#define DRIVERS_KEYBOARD_PROTOCOL_H

#include <arch/types.h> 

//modifier flags
#define KBD_MOD_SHIFT       0x01
#define KBD_MOD_CTRL        0x02
#define KBD_MOD_ALT         0x04

//keyboard event (pushed by driver)
typedef struct {
    uint8  keycode;     //PS/2 scancode
    uint8  mods;        //modifier flags
    uint8  pressed;     //1 = pressed, 0 = released
    uint8  _pad;
    uint32 codepoint;   //Unicode codepoint (0 for non-printable)
} kbd_event_t;

#endif
