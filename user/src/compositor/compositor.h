#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>
#include <keyboard.h>
#include <dm.h>
#include <math.h>
#include "fb.h"
#include "font.h"
#include "cursor.h"
#include <compositor/protocol.h>

#define MAX_SURFACES 16
#define TITLEBAR_H  22
#define BORDER_W     2

#define DECO_TB_FOCUSED    FB_RGB( 22,  24,  40)
#define DECO_TB_UNFOCUSED  FB_RGB( 14,  15,  24)
#define DECO_BD_FOCUSED    FB_RGB( 96, 104, 224)
#define DECO_BD_UNFOCUSED  FB_RGB( 38,  42,  68)
#define DECO_TX_FOCUSED    FB_RGB(220, 225, 255)
#define DECO_TX_UNFOCUSED  FB_RGB( 82,  88, 125)
#define DECO_CLOSE_BTN     FB_RGB(210,  60,  60)

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

#define ASSERT(expr, ...) \
    do { \
        if (expr) { \
            dprintf("\033[31m[compositor]: ERROR: "); \
            dprintf(__VA_ARGS__); \
            dprintf("\033[0m"); \
            exit(1); \
        } \
    } while (0)

#define ERROR(...) \
    do { \
        dprintf("\033[31m[compositor]: ERROR: "); \
        dprintf(__VA_ARGS__); \
        dprintf("\033[0m"); \
    } while (0)

#define WARN(...) \
    do { \
        dprintf("\033[33m[compositor]: WARN: "); \
        dprintf(__VA_ARGS__); \
        dprintf("\033[0m"); \
    } while (0)

#define INFO(...) \
    do { \
        dprintf("[compositor]: INFO: "); \
        dprintf(__VA_ARGS__); \
    } while (0)

typedef struct {
    int16 dx, dy;
    uint8 buttons;
    uint8 _pad[3];
} mouse_event_t;

typedef struct {
    surface_id_t id;
    uint32       owner_pid;
    handle_t     ch;
    uint32      *pixels;
    handle_t     vmo;
    size         vmo_size;
    uint16       w, h;
    int16        x, y;
    uint16       content_x, content_y;
    uint16       content_w, content_h;
    comp_decoration_t deco;
    char         title[32];
    bool         committed;
    bool         focused;
    bool         alive;
} surface_t;

struct compositor_state {
    handle_t fb_handle;
    handle_t server_handle;
    uint32 *backbuffer;
    size fb_size;
    uint16 screen_w, screen_h;
    uint8 screen_bpp;

    //surface registry and z order are tracked separately
    surface_t surfaces[MAX_SURFACES];
    uint8 num_surfaces;
    surface_id_t next_id;

    //wm_ch is out of band and may not belong to a surface
    handle_t wm_ch;
    bool wm_present;

    //mouse state is cached between packets for click edge detection
    handle_t mouse_h;
    int32 mouse_x, mouse_y;
    mouse_event_t mprev;

    bool is_dragging;
    surface_id_t drag_sid;
    int16 drag_offset_x;
    int16 drag_offset_y;

    handle_t vt_handle; //vt handle

    bool keyboard_grabbed;

    uint32 *wallpaper;
    uint16 wallpaper_w, wallpaper_h;
    bool wallpaper_loaded;

    uint32 *saved_fb;

    //stack holds surface ids in bottom to top order
    surface_id_t stack[MAX_SURFACES];
    uint8 stack_count;

    //damage tracking
    bool has_damage;
    int16 damage_x0;
    int16 damage_y0;
    int16 damage_x1;
    int16 damage_y1;

    uint32 screen_pitch_bytes;
    uint32 row_stride;
};

extern struct compositor_state comp;

void send_msg(handle_t ch, comp_msg_t *msg);
int recv_msg(handle_t ch, comp_msg_t *msg, uint32 *sender_pid);

#endif
