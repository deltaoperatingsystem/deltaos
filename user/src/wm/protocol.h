#ifndef WM_PROTOCOL_H
#define WM_PROTOCOL_H

typedef enum {
    ACK,
    CREATE,
    COMMIT,
    DESTROY,
    CONFIGURE,
    RESIZE,
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
    } u;
} wm_server_msg_t;

#endif