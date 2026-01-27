#include <system.h>
#include <io.h>
#include <mem.h>
#include <pixie.h>

typedef struct px_surface {
    uint32 *data;
    uint16 w, h;
    handle_t handle;
} px_surface_t;

typedef struct px_window {
    px_surface_t *surface;
    handle_t ch;
} px_window_t;

#define MAX_TRIES 5

#include "../../src/wm/protocol.h"

static handle_t wm_handle = INVALID_HANDLE;
static handle_t client_handle = INVALID_HANDLE;

/**
 * Acquire a handle to the window manager for subsequent GUI operations.
 *
 * Attempts up to MAX_TRIES to obtain the window manager object at "$gui/wm",
 * yielding between attempts when the handle is not yet available.
 *
 * @returns `true` if the window manager handle was acquired, `false` otherwise.
 */
bool px_init() {
    for (int i = 0; i < MAX_TRIES; i++) {
        wm_handle = get_obj(INVALID_HANDLE, "$gui/wm", RIGHT_READ | RIGHT_WRITE);
        if (wm_handle != INVALID_HANDLE) break;
        yield();
    }
    if (wm_handle == INVALID_HANDLE) return false;
    return true;
}

/**
 * Create a new window and its associated drawable surface registered with the window manager.
 *
 * Sends a CREATE request to the global window manager, waits for acknowledgement and a CONFIGURE
 * message for this process, allocates and maps a VM-backed pixel buffer sized to the configured
 * dimensions, and notifies the window manager of the final surface size.
 *
 * @param name Human-readable window name presented to the window manager.
 * @param width Requested initial surface width in pixels.
 * @param height Requested initial surface height in pixels.
 * @returns Pointer to an initialized px_window_t on success, NULL on failure (e.g., IPC, allocation,
 *          VMO resize or mapping failures).
 */
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

/**
 * Retrieve the surface associated with a window.
 *
 * @param win Window whose surface will be returned.
 * @returns The window's px_surface_t pointer, or `NULL` if `win` is `NULL`.
 */
px_surface_t *px_get_surface(px_window_t *win) {
    if (!win) return NULL;
    return win->surface;
}

/**
 * Get the surface width in pixels.
 * @param surface Surface to query.
 * @returns Width of the surface in pixels, or 0 if `surface` is NULL.
 */
uint16 px_get_surface_w(px_surface_t *surface) {
    if (!surface) return 0;
    return surface->w;
}
/**
 * Get the height of a surface.
 * @returns The surface height in pixels, or 0 if `surface` is NULL.
 */
uint16 px_get_surface_h(px_surface_t *surface) {
    if (!surface) return 0;
    return surface->h;
}

/**
 * Draws a filled rectangle onto a surface.
 *
 * The rectangle is clipped to the surface bounds; if `surface` is NULL or the
 * rectangle's top-left corner lies outside the surface, the call does nothing.
 *
 * @param surface Target surface for drawing.
 * @param r Rectangle to draw; `r.x`/`r.y` specify the top-left pixel, `r.w`/`r.h`
 *          specify width and height in pixels, and `r.c` is the fill color. 
 */
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

/**
 * Request the window manager to commit the window's current surface contents.
 *
 * Sends a commit message for the given window so the window manager can apply
 * the surface's drawn changes. If `win` is NULL the call is a no-op.
 *
 * @param win Window whose surface should be committed; ignored if NULL.
 */
void px_update_window(px_window_t *win) {
    if (!win) return;
    wm_client_msg_t req = (wm_client_msg_t){ .type = COMMIT };
    channel_send(win->ch, &req, sizeof(req));
}