#ifndef COMP_PROTOCOL_H
#define COMP_PROTOCOL_H

#include <keyboard.h>
#include <types.h>

typedef uint32 surface_id_t;

typedef enum {
    MSG_NOP = 0,
    //client->compositor
    MSG_ACK,
    MSG_CREATE_SURFACE,
    MSG_COMMIT,
    MSG_DESTROY_SURFACE,
    MSG_CONFIGURE,
    MSG_RESIZE_SURFACE,
    MSG_KEY_EVENT,
    MSG_MOUSE_EVENT,
    MSG_SET_TITLE,
    MSG_FOCUS_EVENT,
    MSG_DECORATION_EVENT,
    //client->compositor on connect
    MSG_CLIENT_CONNECT,
    //wm <-> compositor
    MSG_CLAIM_WM,
    MSG_UNCLAIM_WM,
    MSG_SET_POSITION,
    MSG_SET_FOCUS,
    MSG_SET_DECORATION,
    MSG_SET_STACKING,
    MSG_SET_KEYBOARD_GRAB,
    MSG_PASS_THROUGH,
    MSG_SET_CLIENT_AREA,
    //compositor->wm
    MSG_SURFACE_CREATED,
    MSG_SURFACE_DESTROYED,
    MSG_SURFACE_ACTIVATED,
    MSG_SURFACE_LIST,
} comp_msg_type_t;

typedef struct {
    uint16 border_w;
    uint16 titlebar_h;
    uint32 tb_focused;
    uint32 tb_unfocused;
    uint32 bd_focused;
    uint32 bd_unfocused;
    uint32 tx_focused;
    uint32 tx_unfocused;
    uint32 close_btn;
    bool   show_close;
} comp_decoration_t;

typedef struct {
    comp_msg_type_t type;
    union {
        struct { uint16 w, h; }                        create_surface;
        struct { surface_id_t id; }                    commit;
        struct { surface_id_t id; }                    destroy_surface;
        struct { surface_id_t id; uint16 w, h; }       resize_surface;
        struct { surface_id_t id; char text[32]; }     set_title;
        struct { surface_id_t id; int16 x, y; uint16 w, h; }  set_position;
        struct { surface_id_t id; }                    set_focus;
        struct { surface_id_t id; comp_decoration_t d; } set_decoration;
        struct { surface_id_t ids[32]; uint8 count; }  set_stacking;
        struct { bool grab; }                          set_keyboard_grab;
        struct { surface_id_t id; kbd_event_t data; }  pass_through;
        struct { surface_id_t id; uint16 x, y, w, h; } set_client_area;

        struct { bool ok; surface_id_t id; }           ack;
        struct { surface_id_t id; uint16 x, y, w, h, bpp; } configure;
        struct { surface_id_t id; int16 x, y; uint8 buttons; } mouse_event;
        struct { surface_id_t id; kbd_event_t data; }  key_event;
        struct { surface_id_t id; bool focused; }      focus_event;
        struct { surface_id_t id; uint8 type; }        decoration_event;
        struct { surface_id_t id; uint32 pid; uint16 w, h; char title[32]; } surface_created;
        struct { surface_id_t id; }                    surface_destroyed;
        struct { surface_id_t id; }                    surface_activated;
        struct { surface_id_t ids[64]; uint8 count; }  surface_list;
    } u;
} comp_msg_t;

#endif
