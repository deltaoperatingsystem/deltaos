#ifndef WM_PROTOCOL_H
#define WM_PROTOCOL_H

#include <keyboard.h>

typedef enum {
    ACK,
    CREATE,
    COMMIT,
    DESTROY,
    CONFIGURE,
    RESIZE,
    KBD,
    MOUSE,
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
        struct {
            int16 x, y;
            uint8 buttons;
        } mouse;
    } u;
} wm_server_msg_t;

typedef struct {
    uint32 pid;
    handle_t handle;
    uint32 *surface;
    handle_t vmo;
    uint16 surface_w, surface_h;
    uint16 win_w, win_h;
    uint16 x, y;
    bool dirty;
    enum {
        EMPTY,
        CREATED,
        CONFIGURED,
        READY,
        DEAD,
    } status;
} wm_client_t;

#endif