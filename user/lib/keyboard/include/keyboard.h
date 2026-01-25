#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <types.h>

//keyboard event structure (matches kernel kbd_event_t)
typedef struct {
    uint8  keycode;
    uint8  mods;
    uint8  pressed;
    uint8  _pad;
    uint32 codepoint;
} kbd_event_t;

//modifier flags
#define KBD_MOD_SHIFT   (1 << 0)
#define KBD_MOD_CTRL    (1 << 1)
#define KBD_MOD_ALT     (1 << 2)

//initialize keyboard - opens channel
//returns 0 on success, -1 on failure
int kbd_init(void);

//blocking read - waits for keypress
//returns 0 on success, -1 on error
int kbd_read(kbd_event_t *event);

//non-blocking read
//returns 0 if event read, -1 if no event available
int kbd_try_read(kbd_event_t *event);

//flush all pending events
void kbd_flush(void);

//close keyboard channel
void kbd_close(void);

//convenience: read a character (blocking, printable only)
//returns character or 0 on error
char kbd_getchar(void);

#endif
