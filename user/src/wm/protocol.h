#ifndef WM_PROTOCOL_H
#define WM_PROTOCOL_H

#include "../../libkeyboard/include/keyboard.h"

typedef enum {
    ACK,
    CREATE,
    COMMIT,
    DESTROY,
    CONFIGURE,
    RESIZE,
    KBD,
} wm_msg_type_t;

typedef struct {
    wm_msg_type_t type;
    union {
        struct {
            uint16 width, height;
        } create;
        struct {
            uint16 width, height;
        } resize;
    } u;
} wm_client_msg_t;

typedef struct {
    wm_msg_type_t type;
    union {
        bool ack;
        struct {
            uint16 x, y, w, h;
        } configure;
        struct {
            kbd_event_t data;
        } kbd;
    } u;
} wm_server_msg_t;

#endif