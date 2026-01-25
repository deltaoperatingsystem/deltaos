#include <system.h>
#include <io.h>
#include <mem.h>

typedef struct px_surface {
    uint32 *data;
    uint16 w, h;
    handle_t handle;
} px_surface_t;

typedef struct px_window {
    px_surface_t *surface;
    handle_t ch;
} px_window_t;

typedef struct px_rect {
    uint16 x, y;
    uint16 w, h;
    uint32 c;
} px_rect_t;

#include "../../src/wm/protocol.h"

static handle_t wm_handle = INVALID_HANDLE;
static handle_t client_handle = INVALID_HANDLE;

void px_init() {
    wm_handle = get_obj(INVALID_HANDLE, "$gui/wm", RIGHT_READ | RIGHT_WRITE);
}

px_window_t *px_create_window(char *name, uint16 width, uint16 height) {
    wm_client_msg_t req = (wm_client_msg_t){
        .type = CREATE,
        .u.create.width = width,
        .u.create.height = height,
    };
    channel_send(wm_handle, &req, sizeof(req));
    wm_server_msg_t res;
    channel_recv(wm_handle, &res, sizeof(res));
    if (res.type != ACK) return NULL;
    
    px_window_t *win = malloc(sizeof(px_window_t));
    if (!win) return NULL;

    channel_recv(wm_handle, &res, sizeof(res));
    if (res.type != CONFIGURE) return NULL;

    char path[64];
    snprintf(path, sizeof(path), "$gui/%d/surface", getpid());
    
    win->surface = malloc(sizeof(px_surface_t));
    if (!win->surface) return NULL;

    win->surface->h = res.u.configure.w;
    win->surface->w = res.u.configure.h;
    
    win->surface->handle = get_obj(INVALID_HANDLE, path, RIGHT_WRITE | RIGHT_MAP);
    win->surface->data = vmo_map(win->surface->handle, NULL, 0,
        win->surface->w * win->surface->h * sizeof(uint32), RIGHT_WRITE | RIGHT_MAP);
    
    snprintf(path, sizeof(path), "$gui/%d/channel", getpid());
    win->ch = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_WRITE);

    // req = (wm_client_msg_t){ .type = RESIZE, .u.resize.width = win->surface->w, .u.resize.height = win->surface->h };
    // channel_send(win->ch, &req, sizeof(req));

    return win;
}

px_surface_t *px_get_surface(px_window_t *win) {
    if (!win) return NULL;
    return win->surface;
}

int16 px_get_surface_w(px_surface_t *surface) {
    if (!surface) return -1;
    return surface->w;
}

int16 px_get_surface_h(px_surface_t *surface) {
    if (!surface) return -1;
    return surface->h;
}

void px_draw_pixel(px_surface_t *surface, uint32 x, uint32 y, uint32 c) {
    if (!surface) return;
    if (x >= surface->w || y >= surface->h) return;

    surface->data[y * surface->w + x] = c;
}

void px_draw_rect(px_surface_t *surface, px_rect_t *r) {
    if (!surface || !r) return;

    //clamp to surface bounds
    if (r->x >= surface->w || r->y >= surface->h) return;
    if (r->x + r->w > surface->w) r->x = surface->w - r->x;
    if (r->y + r->h > surface->h) r->h = surface->h - r->y;

    for (uint32 i = 0; i < r->w; i++) {
        for (uint32 j = 0; j < r->h; j++) {
            px_draw_pixel(surface, r->x + i, r->y + j, r->c);
        }
    }
}

// typedef struct px_rect {
//     uint16 x, y;
//     uint16 w, h;
//     uint32 c;
// } px_rect_t;

px_rect_t *px_create_rect(uint16 x, uint16 y, uint16 w, uint16 h, uint16 c) {
    return &(px_rect_t){ .x = x, .y = y, .w = w, .h = h, .c = c };
}

void px_update_window(px_window_t *win) {
    if (!win) return;
    wm_client_msg_t req = (wm_client_msg_t){ .type = COMMIT };
    channel_send(win->ch, &req, sizeof(req));
}