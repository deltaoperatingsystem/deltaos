#include <system.h>
#include <io.h>
#include <mem.h>
#include <pixie.h>
#include <dm.h>

typedef struct px_surface {
    uint32 *data;
    uint16 w, h;
    bool dirty;
    handle_t handle;
} px_surface_t;

typedef struct px_window {
    px_surface_t *surface;
    handle_t ch;
} px_window_t;

typedef struct px_image {
    uint32 width, height;
    uint8 pixel_format;
    uint8 bpp;
    uint8 *pixels;
} px_image_t;

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
    if (!win->surface) {
        free(win);
        return NULL;
    }

    win->surface->dirty = false;
    win->surface->w = res.u.configure.w;
    win->surface->h = res.u.configure.h;
    
    win->surface->handle = get_obj(INVALID_HANDLE, path, RIGHT_WRITE | RIGHT_MAP);
    
    size surface_size = (size)win->surface->w * (size)win->surface->h * sizeof(uint32);
    vmo_resize(win->surface->handle, surface_size);
    win->surface->data = vmo_map(win->surface->handle, NULL, 0, surface_size, RIGHT_WRITE | RIGHT_MAP);
    if (!win->surface->data) {
        free(win->surface); free(win);
        return NULL;
    }

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
    // trivial clamp: if top-left outside surface, skip
    if (r.x >= surface->w || r.y >= surface->h) return;

    // clamp width/height
    if (r.x + r.w > surface->w) r.w = surface->w - r.x;
    if (r.y + r.h > surface->h) r.h = surface->h - r.y;

    for (uint32 yy = 0; yy < r.h; yy++) {
        uint32 base = (r.y + yy) * surface->w + r.x;
        for (uint32 xx = 0; xx < r.w; xx++) {
            surface->data[base + xx] = r.c;
        }
    }

    surface->dirty = true;
}

void px_update_window(px_window_t *win) {
    if (!win) return;
    if (!win->surface->dirty) return;
    wm_client_msg_t req = (wm_client_msg_t){ .type = COMMIT };
    channel_send(win->ch, &req, sizeof(req));
}

px_image_t *px_load_image(char *path) {
    handle_t h = get_obj(INVALID_HANDLE, path, RIGHT_READ);
    if (h == INVALID_HANDLE) return NULL;
    
    stat_t st;
    fstat(h, &st);
    
    uint8 *data = malloc(st.size);
    handle_read(h, data, st.size);
    
    dm_image_t image;
    int err = dm_load_image(data, st.size, &image);
    free(data);
    if (err != 0) { handle_close(h); return NULL; }

    px_image_t *out = malloc(sizeof(px_image_t));
    if (!out) { free(image.pixels); handle_close(h); return NULL; };
    //cause C doesnt let you cast structs
    out->width = image.width;
    out->height = image.height;
    out->pixels = image.pixels;
    out->pixel_format = image.pixel_format;
    out->bpp = image.bpp;
    handle_close(h);

    return out;
}

bool px_draw_pixel(px_surface_t *surface, uint32 x, uint32 y, uint32 colour) {
    if (!surface) return false;
    if (x >= surface->w || y >= surface->h) return false;
    surface->data[y * surface->w + x] = colour;
    surface->dirty = true;
    return true;
}

bool px_draw_image(px_surface_t *surface, px_image_t *image, uint32 x, uint32 y) {
    if (!surface) return false;
    if (!image) return false;
    px_image_t src = *image;
    for (uint32 i = 0; i < src.height; i++) {
        for (uint32 j = 0; j < src.width; j++) {
            uint8 r, g, b;

            switch (image->pixel_format) {
                case DM_PIXEL_RGBA32: {
                    r = *src.pixels++;
                    g = *src.pixels++;
                    b = *src.pixels++;
                    src.pixels++; // ignore extra A channel
                    break;
                }
                default: return false;
            }

            uint32 colour = PX_RGB(r, g, b);
            px_draw_pixel(surface, j + x, i + y, colour);
        }
    }

    surface->dirty = true;
    return true;
}
