#ifndef DRIVERS_MOUSE_PROTOCOL_H
#define DRIVERS_MOUSE_PROTOCOL_H

#include <arch/types.h>

//mouse button flags
#define MOUSE_BTN_LEFT      0x01
#define MOUSE_BTN_RIGHT     0x02
#define MOUSE_BTN_MIDDLE    0x04

//mouse event (pushed by driver)
typedef struct {
    int16 dx; //x movement delta
    int16 dy; //y movement delta
    uint8 buttons; //button state (MOUSE_BTN_*)
    uint8 _pad[3];
} mouse_event_t;

#endif
