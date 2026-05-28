#include <system.h>
#include <io.h>
#include <mem.h>
#include <pixie.h>
#include <dm.h>
#include <compositor/protocol.h>
#include <compositor/client.h>

typedef struct px_surface {
    uint32 *data;
    uint16 w, h, bpp;
    bool dirty;
    handle_t handle;
    size mapped_sz; //actual byte count passed to vmo_map (may be > w*h*bpp due to page rounding)
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

#define MAX_TRIES 50

static handle_t server_handle = INVALID_HANDLE;

static void px_send_resize(px_window_t *win) {
    if (!win || !win->surface) return;
    comp_resize_surface(win->ch, 0, win->surface->w, win->surface->h);
}

//drop the old mapping before resize so the new map can land anywhere cleanly
static bool px_remap_surface(px_window_t *win, uint16 new_w, uint16 new_h, uint16 new_bpp, bool notify_compositor) {
    if (!win || !win->surface) return false;

    px_surface_t *surface = win->surface;

    //use the stored mapped_sz so vmo_unmap covers exactly what vmo_map allocated
    if (surface->data && surface->mapped_sz > 0) {
        vmo_unmap(surface->data, surface->mapped_sz);
        surface->data = NULL;
        surface->mapped_sz = 0;
    }

    size new_sz = (size)new_w * (size)new_h * (size)new_bpp;
    if (vmo_resize(surface->handle, new_sz) != 0) return false;

    surface->data = vmo_map(surface->handle, NULL, 0, new_sz, RIGHT_WRITE | RIGHT_MAP);
    if (!surface->data) return false;

    //vmo_map rounds len up to a page boundary - record the actual allocation size
    surface->mapped_sz = (new_sz + 0xFFF) & ~(size)0xFFF;
    surface->w = new_w;
    surface->h = new_h;
    surface->bpp = new_bpp;
    surface->dirty = false;

    if (notify_compositor) px_send_resize(win);
    return true;
}

bool px_init() {
    if (server_handle != INVALID_HANDLE) return true;

    server_handle = comp_connect();
    if (server_handle == INVALID_HANDLE) return false;

    comp_msg_t msg = { .type = MSG_CLIENT_CONNECT };
    channel_send(server_handle, &msg, sizeof(msg));

    bool got_ack = false;
    for (int i = 0; i < MAX_TRIES; i++) {
        comp_msg_t resp;
        int rc = channel_try_recv(server_handle, &resp, sizeof(resp));
        if (rc == (ssize)sizeof(resp) && resp.type == MSG_ACK) {
            got_ack = resp.u.ack.ok;
            break;
        }
        yield();
    }

    if (!got_ack) {
        handle_close(server_handle);
        server_handle = INVALID_HANDLE;
        return false;
    }
    return true;
}

px_window_t *px_create_window(char *name, uint16 width, uint16 height) {
    (void)name;

    surface_id_t id;
    handle_t ch;
    if (!comp_create_surface(server_handle, width, height, &id, &ch)) {
        return NULL;
    }

    px_window_t *win = malloc(sizeof(px_window_t));
    if (!win) {
        comp_destroy_surface(ch, id);
        handle_close(ch);
        return NULL;
    }
    win->ch = ch;

    comp_msg_t cfg = {0};
    bool got_cfg = false;
    for (int i = 0; i < MAX_TRIES; i++) {
        int rc = channel_try_recv(win->ch, &cfg, sizeof(cfg));
        if (rc == (ssize)sizeof(cfg) && cfg.type == MSG_CONFIGURE) {
            got_cfg = true;
            break;
        }
        yield();
    }
    if (!got_cfg) {
        comp_destroy_surface(win->ch, id);
        handle_close(win->ch);
        free(win);
        return NULL;
    }

    char path[64];
    snprintf(path, sizeof(path), "$gui/display/%d_%d/surface", getpid(), id);
    win->surface = malloc(sizeof(px_surface_t));
    if (!win->surface) {
        comp_destroy_surface(win->ch, id);
        handle_close(win->ch);
        free(win);
        return NULL;
    }

    win->surface->data = NULL;
    win->surface->mapped_sz = 0;
    win->surface->dirty = false;
    win->surface->w = cfg.u.configure.w;
    win->surface->h = cfg.u.configure.h;
    win->surface->bpp = cfg.u.configure.bpp;

    win->surface->handle = get_obj(INVALID_HANDLE, path, RIGHT_WRITE | RIGHT_MAP);
    if (win->surface->handle == INVALID_HANDLE) {
        free(win->surface);
        comp_destroy_surface(win->ch, id);
        handle_close(win->ch);
        free(win);
        return NULL;
    }

    if (!px_remap_surface(win, cfg.u.configure.w, cfg.u.configure.h, cfg.u.configure.bpp, false)) {
        handle_close(win->surface->handle);
        free(win->surface);
        comp_destroy_surface(win->ch, id);
        handle_close(win->ch);
        free(win);
        return NULL;
    }

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
    if (!surface->data) return;
    if (r.x >= surface->w || r.y >= surface->h) return;
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
    if (!win || !win->surface) return;
    if (!win->surface->dirty) return;
    comp_commit(win->ch, 0);
    win->surface->dirty = false;
}

px_image_t *px_load_image(char *path) {
    handle_t h = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_GET_INFO);
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
    if (!out) { free(image.pixels); handle_close(h); return NULL; }
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
    if (!surface->data) return false;
    if (x >= surface->w || y >= surface->h) return false;
    surface->data[y * surface->w + x] = colour;
    surface->dirty = true;
    return true;
}

bool px_draw_image(px_surface_t *surface, px_image_t *image, uint32 x, uint32 y) {
    if (!surface || !image) return false;
    px_image_t src = *image;
    for (uint32 i = 0; i < src.height; i++) {
        for (uint32 j = 0; j < src.width; j++) {
            uint8 r, g, b;
            switch (image->pixel_format) {
                case DM_PIXEL_RGBA32: {
                    r = *src.pixels++;
                    g = *src.pixels++;
                    b = *src.pixels++;
                    src.pixels++;
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

bool px_set_title(px_window_t *win, const char *title) {
    if (!win || !title) return false;
    comp_set_title(win->ch, 0, title);
    return true;
}

bool px_poll_event(px_window_t *win, px_event_t *ev) {
    if (!win || !ev) return false;
    ev->type = PX_EVENT_NONE;

    //skip compositor internal messages until we either expose one or run dry
    while (1) {
        comp_msg_t msg;
        int rc = channel_try_recv(win->ch, &msg, sizeof(msg));
        if (rc != (ssize)sizeof(msg)) return false;

        switch (msg.type) {
            case MSG_CONFIGURE: {
                uint16 new_w = msg.u.configure.w;
                uint16 new_h = msg.u.configure.h;
                uint16 new_bpp = msg.u.configure.bpp;

                if (!px_remap_surface(win, new_w, new_h, new_bpp, true))
                    return false;

                ev->type = PX_EVENT_RESIZE;
                ev->resize.w = new_w;
                ev->resize.h = new_h;
                return true;
            }

            case MSG_MOUSE_EVENT:
                ev->type = PX_EVENT_MOUSE;
                ev->mouse.x       = msg.u.mouse_event.x;
                ev->mouse.y       = msg.u.mouse_event.y;
                ev->mouse.buttons = msg.u.mouse_event.buttons;
                return true;

            case MSG_KEY_EVENT:
                ev->type = PX_EVENT_KEY;
                ev->key.data = msg.u.key_event.data;
                return true;

            case MSG_FOCUS_EVENT:
                //focus toggles are internal for now so keep draining
                continue;

            default:
                continue;
        }
    }
}
