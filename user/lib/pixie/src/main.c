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

#define MAX_TRIES 5

#include "../../src/wm/protocol.h"

static handle_t wm_handle = INVALID_HANDLE;
static handle_t client_handle = INVALID_HANDLE;

bool px_init() {
    for (int i = 0; i < MAX_TRIES; i++) {
        wm_handle = get_obj(INVALID_HANDLE, "$gui/wm", RIGHT_READ | RIGHT_WRITE);
        if (wm_handle != INVALID_HANDLE) break;
        yield();
    }
    if (wm_handle == INVALID_HANDLE) return false;
    return true;
}

px_window_t *px_create_window(char *name, uint16 width, uint16 height) {
    wm_client_msg_t req = (wm_client_msg_t){
        .type = CREATE,
        .u.create.width = width,
        .u.create.height = height,
    };
    channel_send(wm_handle, &req, sizeof(req));
    wm_server_msg_t res;
    for (int i = 0; i < MAX_TRIES; i++) {
        channel_recv(wm_handle, &res, sizeof(res));
        if (res.type == ACK) break;
        yield();
    } if (res.type != ACK) return NULL;
    
    px_window_t *win = malloc(sizeof(px_window_t));
    if (!win) return NULL;

    char path[64];
    snprintf(path, sizeof(path), "$gui/%d/channel", getpid());
    win->ch = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_WRITE);

    for (int i = 0; i < MAX_TRIES; i++) {
        channel_recv(win->ch, &res, sizeof(res));
        if (res.type == CONFIGURE) break;
        yield();
    } if (res.type != CONFIGURE) return NULL;

    snprintf(path, sizeof(path), "$gui/%d/surface", getpid());
    win->surface = malloc(sizeof(px_surface_t));
    if (!win->surface) return NULL;

    win->surface->w = res.u.configure.w;
    win->surface->h = res.u.configure.h;
    
    win->surface->handle = get_obj(INVALID_HANDLE, path, RIGHT_WRITE | RIGHT_MAP);
    
    vmo_resize(win->surface->handle, win->surface->w * win->surface->h * sizeof(uint32));
    win->surface->data = vmo_map(win->surface->handle, NULL, 0,
        win->surface->w * win->surface->h * sizeof(uint32), RIGHT_WRITE | RIGHT_MAP);

    req = (wm_client_msg_t){ .type = RESIZE, .u.resize.width = win->surface->w, .u.resize.height = win->surface->h };
    channel_send(win->ch, &req, sizeof(req));

    return win;
}

px_surface_t *px_get_surface(px_window_t *win) {
    if (!win) return NULL;
    return win->surface;
}

uint16 px_get_surface_w(px_surface_t *surface) {
    if (!surface) return 0;
    return surface->w;
}
uint16 px_get_surface_h(px_surface_t *surface) {
    if (!surface) return 0;
    return surface->h;
}

void px_draw_rect(px_surface_t *surface, px_rect_t r) {
    if (!surface) return;
    /* trivial clamp: if top-left outside surface, skip */
    if (r.x >= surface->w || r.y >= surface->h) return;

    /* clamp width/height */
    if (r.x + r.w > surface->w) r.w = surface->w - r.x;
    if (r.y + r.h > surface->h) r.h = surface->h - r.y;

    for (uint32 yy = 0; yy < r.h; yy++) {
        uint32 base = (r.y + yy) * surface->w + r.x;
        for (uint32 xx = 0; xx < r.w; xx++) {
            surface->data[base + xx] = r.c;
        }
    }
}

void px_update_window(px_window_t *win) {
    if (!win) return;
    wm_client_msg_t req = (wm_client_msg_t){ .type = COMMIT };
    channel_send(win->ch, &req, sizeof(req));
}