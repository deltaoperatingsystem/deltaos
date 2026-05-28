#ifndef PIXIE_H
#define PIXIE_H

#include <types.h>
#include <keyboard.h>

typedef struct px_surface px_surface_t;
typedef struct px_window  px_window_t;

typedef struct px_rect {
    uint16 x, y;
    uint16 w, h;
    uint32 c;
} px_rect_t;

typedef struct px_image px_image_t;

#define PX_RGB(r, g, b) ((uint32)((b) | ((g) << 8) | ((r) << 16)))

//mouse button flags, matches compositor internal flags
#define PX_MOUSE_BTN_LEFT   0x01
#define PX_MOUSE_BTN_RIGHT  0x02
#define PX_MOUSE_BTN_MIDDLE 0x04

//event types
typedef enum {
    PX_EVENT_NONE = 0,
    PX_EVENT_MOUSE,   //mouse moved / button changed
    PX_EVENT_KEY,     //key pressed or released
    PX_EVENT_RESIZE,  //window was resized by the WM
} px_event_type_t;

typedef struct {
    px_event_type_t type;
    union {
        struct {
            int16  x, y;      //absolute coords within content area
            uint8  buttons;   //px_mouse_btn_* bitmask
        } mouse;
        struct {
            kbd_event_t data;
        } key;
        struct {
            uint16 w, h;
        } resize;
    };
} px_event_t;

//lifecycle
bool        px_init(void);
px_window_t *px_create_window(char *name, uint16 width, uint16 height);

//surface access
px_surface_t *px_get_surface(px_window_t *win);
uint16        px_get_surface_w(px_surface_t *surface);
uint16        px_get_surface_h(px_surface_t *surface);

//drawing
static inline px_rect_t px_create_rect(uint16 x, uint16 y, uint16 w, uint16 h, uint32 c) {
    return (px_rect_t){.x=x,.y=y,.w=w,.h=h,.c=c};
}
bool px_draw_pixel(px_surface_t *surface, uint32 x, uint32 y, uint32 colour);
void px_draw_rect (px_surface_t *surface, px_rect_t rect);

px_image_t *px_load_image(char *path);
bool        px_draw_image(px_surface_t *surface, px_image_t *image, uint32 x, uint32 y);

//window management
void px_update_window(px_window_t *win);

//set window title shown in title bar, max 31 chars
bool px_set_title(px_window_t *win, const char *title);

//non-blocking event poll, handles configure internally
bool px_poll_event(px_window_t *win, px_event_t *ev);

#endif