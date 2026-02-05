// wm.c - improved logging and hardening
#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>
#include <keyboard.h>
#include <dm.h>
#include "protocol.h"
#include "cursor.h"
#include "fb.h"

#ifndef DEBUG
static bool debug = false;
#else
static bool debug = true;
#endif

#define FB_BACKBUFFER_SIZE  (1280 * 800 * sizeof(uint32))

#define SCREEN_WIDTH    1280
#define SCREEN_HEIGHT   800

#define ASSERT(expr, msg, ...) do { if (expr) { dprintf("\033[31m[wm]: ERROR: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); return; } } while (0)
#define ERROR(msg, ...) do { dprintf("\033[31m[wm]: ERROR: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); } while (0)
#define WARN(msg, ...) do { if (debug == true) { dprintf("\033[33m[wm]: WARN: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); } } while (0)
#define INFO(msg, ...) do { if (debug == true) { dprintf("[wm]: INFO: "); dprintf(msg, ##__VA_ARGS__); } } while (0)
#define LOG_ERR_RET(expr, msg, ...) do { if (expr) { dprintf("\033[31m[wm]: ERROR: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); return -1; } } while (0)

static inline int max(int a, int b) {
    return (a > b) ? a : b;
}

static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

static void client_remove_at(int idx);
static void recompute_layout(uint16 screen_w, uint16 screen_h);
static void client_teardown_by_index(int idx);

static struct {
    uint32 *pixels;
    uint16 width;
    uint16 height;
    bool loaded;
} wallpaper = { .pixels = NULL, .width = 0, .height = 0, .loaded = false };

static int load_wallpaper(void);
static void free_wallpaper(void);
static void render_wallpaper(uint32 *fb_backbuffer);

void fb_setup(handle_t *h, uint32 **backbuffer) {
    *h = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_READ | RIGHT_WRITE);
    ASSERT(*h == INVALID_HANDLE, "Failed to get framebuffer handle\n");
    *backbuffer = malloc(FB_BACKBUFFER_SIZE);
    ASSERT(!*backbuffer, "Failed to allocate global surface\n");
    INFO("Framebuffer and backbuffer setup OK (handle=%d)\n", (int)*h);
}

void server_setup(handle_t *server, handle_t *client) {
    channel_create(server, client);
    ns_register("$gui/wm", *client);
    INFO("Server channel published at $gui/wm\n");
}

wm_client_t clients[16];
uint8 num_clients = 0;
int8 focused = -1;

static void client_remove_at(int idx) {
    if (idx < 0 || idx >= num_clients) return;

    wm_client_t *c = &clients[idx];
    INFO("Tearing down client idx=%d pid=%u\n", idx, c->pid);
    c->status = DEAD;

    if (c->surface) {
        vmo_unmap(c->surface, (size)c->surface_w * (size)c->surface_h * sizeof(uint32));
        c->surface = NULL;
    }

    if (c->vmo != INVALID_HANDLE) {
        handle_close(c->vmo);
        c->vmo = INVALID_HANDLE;
    }

    if (c->handle != INVALID_HANDLE) {
        handle_close(c->handle);
        c->handle = INVALID_HANDLE;
    }

    for (int j = idx; j + 1 < num_clients; j++) {
        clients[j] = clients[j + 1];
    }
    num_clients--;

    if (num_clients == 0) {
        focused = -1;
    } else if (focused >= num_clients) {
        focused = num_clients - 1;
    }

    c->status = EMPTY;
    recompute_layout(SCREEN_WIDTH, SCREEN_HEIGHT);
    INFO("Client removed. remaining=%d focused=%d\n", num_clients, focused);
}

static void client_teardown_by_index(int idx) {
    WARN("client_teardown_by_index: tearing down client at index %d\n", idx);
    client_remove_at(idx);
}

static void recompute_layout(uint16 screen_w, uint16 screen_h) {
    if (num_clients == 0) return;
    INFO("Recomputing layout for %u clients (screen %u x %u)\n", num_clients, screen_w, screen_h);

    uint16 tile_h = screen_h / num_clients;
    for (int i = 0; i < num_clients; i++) {
        wm_client_t *c = &clients[i];
        c->x = 0;
        c->y = i * tile_h;
        c->win_w = screen_w;
        c->win_h = (i == num_clients - 1) ? (screen_h - tile_h * (num_clients - 1)) : tile_h;
        c->dirty = true;

        wm_server_msg_t configure = (wm_server_msg_t){
            .type = CONFIGURE,
            .u.configure = {
                .x = (uint16)c->x,
                .y = (uint16)c->y,
                .w = (uint16)c->win_w,
                .h = (uint16)c->win_h
            }
        };

        int rc = channel_send(c->handle, &configure, sizeof(configure));
        if (rc != 0) {
            WARN("Failed to send CONFIGURE to pid=%u (idx=%d) rc=%d, scheduling teardown\n", c->pid, i, rc);
            client_teardown_by_index(i);
            i--;
            continue;
        } else {
            INFO("Sent CONFIGURE to pid=%u idx=%d => x=%d y=%d w=%d h=%d\n",
                 c->pid, i, c->x, c->y, c->win_w, c->win_h);
            c->status = CONFIGURED;
        }
    }
}

void window_create(handle_t *server, channel_recv_result_t res, wm_client_msg_t req) {
    INFO("window_create from pid=%u requested %u x %u\n", res.sender_pid, req.u.create.width, req.u.create.height);
    if (num_clients == 16) {
        WARN("Max clients reached, ignoring create from pid=%u\n", res.sender_pid);
        return;
    }

    char path[64];
    size needed = (size)req.u.create.width * (size)req.u.create.height * sizeof(uint32);
    handle_t client_vmo = vmo_create(needed, VMO_FLAG_RESIZABLE, RIGHT_MAP);
    if (client_vmo == INVALID_HANDLE) {
        ERROR("vmo_create failed for pid=%u size=%zu\n", res.sender_pid, needed);
        return;
    }
    snprintf(path, sizeof(path), "$gui/%u/surface", res.sender_pid);
    ns_register(path, client_vmo);
    INFO("Registered surface %s for pid=%u\n", path, res.sender_pid);

    uint32 *surface = vmo_map(client_vmo, NULL, 0, needed, RIGHT_MAP);
    if (!surface) {
        ERROR("vmo_map failed for pid=%u size=%zu\n", res.sender_pid, needed);
        handle_close(client_vmo);
        return;
    }

    handle_t wm_end = INVALID_HANDLE, client_end = INVALID_HANDLE;
    channel_create(&wm_end, &client_end);
    if (wm_end == INVALID_HANDLE || client_end == INVALID_HANDLE) {
        ERROR("channel_create failed for pid=%u\n", res.sender_pid);
        vmo_unmap(surface, needed);
        handle_close(client_vmo);
        return;
    }
    snprintf(path, sizeof(path), "$gui/%u/channel", res.sender_pid);
    ns_register(path, client_end);
    INFO("Published channel %s for pid=%u\n", path, res.sender_pid);

    wm_client_t newc = {
        .pid = res.sender_pid,
        .handle = wm_end,
        .surface = surface,
        .vmo = client_vmo,
        .surface_w = req.u.create.width,
        .surface_h = req.u.create.height,
        .win_w = req.u.create.width,
        .win_h = req.u.create.height,
        .x = 0,
        .y = 0,
        .dirty = false,
        .status = CREATED,
    };
    clients[num_clients++] = newc;

    wm_server_msg_t resp = (wm_server_msg_t){ .type = ACK, .u.ack = true };
    channel_send(*server, &resp, sizeof(resp));
    INFO("CREATE handled for pid=%u idx=%d\n", res.sender_pid, num_clients - 1);

    if (focused == -1) focused = 0;
    recompute_layout(SCREEN_WIDTH, SCREEN_HEIGHT);
}

void window_commit(channel_recv_result_t res) {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].pid == res.sender_pid) {
            INFO("Received COMMIT from pid=%u idx=%d\n", res.sender_pid, i);
            if (clients[i].status != READY) {
                WARN("Ignoring COMMIT from pid=%u because status=%d\n", res.sender_pid, clients[i].status);
                return;
            }

            clients[i].dirty = true;
            return;
        }
    }
    WARN("COMMIT from unknown pid=%u\n", res.sender_pid);
}

void server_listen(handle_t *server) {
    wm_client_msg_t msg;
    channel_recv_result_t res;

    int s_rc = channel_try_recv_msg(*server, &msg, sizeof(wm_client_msg_t), NULL, 0, &res);
    if (s_rc == 0) {
        switch (msg.type) {
            case CREATE:
                INFO("Received CREATE on $gui/wm from pid=%u\n", res.sender_pid);
                window_create(server, res, msg);
                break;
            default:
                WARN("Unknown message type %d on $gui/wm\n", msg.type);
                break;
        }
    } else if (s_rc < 0 && s_rc != -3) {
        WARN("Error receiving on $gui/wm rc=%d\n", s_rc);
    }

    for (int i = 0; i < num_clients; i++) {
        wm_client_msg_t cmsg;
        channel_recv_result_t cres;
        int rc = channel_try_recv_msg(clients[i].handle, &cmsg, sizeof(wm_client_msg_t), NULL, 0, &cres);
        if (rc != 0) {
            if (rc < 0 && rc != -3) {
                WARN("Channel error or peer closed for pid=%u idx=%d rc=%d - tearing down\n", clients[i].pid, i, rc);
                client_remove_at(i--);
            }
            continue;
        }

        switch (cmsg.type) {
            case ACK:
                INFO("ACK recv from pid=%u idx=%d status=%d\n", clients[i].pid, i, clients[i].status);
                break;

            case COMMIT:
                window_commit(cres);
                break;

            case RESIZE:
                INFO("RESIZE recv from pid=%u requested %u x %u\n", clients[i].pid, cmsg.u.resize.width, cmsg.u.resize.height);
                if (clients[i].status != CONFIGURED) {
                    WARN("Ignoring RESIZE from pid=%u because status=%d\n", clients[i].pid, clients[i].status);
                    break;
                }

                if (clients[i].surface) {
                    vmo_unmap(clients[i].surface, (size)clients[i].surface_w * (size)clients[i].surface_h * sizeof(uint32));
                    clients[i].surface = NULL;
                }
                clients[i].surface_w = cmsg.u.resize.width;
                clients[i].surface_h = cmsg.u.resize.height;
                size needed = (size)clients[i].surface_w * (size)clients[i].surface_h * sizeof(uint32);
                clients[i].surface = vmo_map(clients[i].vmo, NULL, 0, needed, RIGHT_MAP);
                if (!clients[i].surface) {
                    ERROR("vmo_map failed for pid=%u idx=%d\n", clients[i].pid, i);
                    client_remove_at(i--);
                    break;
                }
                clients[i].dirty = true;
                INFO("Resized and remapped pid=%u idx=%d -> %u x %u\n", clients[i].pid, i, clients[i].surface_w, clients[i].surface_h);
                clients[i].status = READY;
                break;

            case KBD:
                WARN("Unexpected KBD message from client pid=%u\n", clients[i].pid);
                break;

            case DESTROY:
                clients[i].status = DEAD;
                client_remove_at(i--);
                break;

            default:
                WARN("Unknown client msg type=%d from pid=%u\n", cmsg.type, clients[i].pid);
                break;
        }
    }
}

static void scale_nearest_neighbour(const uint32 *src, int sw, int sh, uint32 *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            if (sx >= sw) sx = sw - 1;
            dst[y * dw + x] = src[sy * sw + sx];
        }
    }
}

static int load_wallpaper(void) {
    handle_t h = get_obj(INVALID_HANDLE, "$files/wallpaper.dm", RIGHT_READ);
    if (h == INVALID_HANDLE) {
        WARN("No wallpaper present at $files/wallpaper.dm; using solid background\n");
        wallpaper.loaded = false;
        return -1;
    }

    stat_t st;
    if (fstat(h, &st) != 0) {
        WARN("fstat on wallpaper handle failed; using solid background\n");
        handle_close(h);
        wallpaper.loaded = false;
        return -1;
    }

    uint8 *filebuf = malloc(st.size);
    if (!filebuf) {
        WARN("malloc for wallpaper file failed\n");
        handle_close(h);
        wallpaper.loaded = false;
        return -1;
    }
    if (handle_read(h, filebuf, st.size) != (ssize)st.size) {
        WARN("handle_read for wallpaper failed\n");
        free(filebuf);
        handle_close(h);
        wallpaper.loaded = false;
        return -1;
    }
    handle_close(h);

    dm_image_t img;
    if (dm_load_image(filebuf, st.size, &img) != 0) {
        WARN("dm_load_image failed for wallpaper; using solid background\n");
        free(filebuf);
        wallpaper.loaded = false;
        return -1;
    }

    size img_pixels = (size)img.width * (size)img.height * sizeof(uint32);
    if (img.width == 0 || img.height == 0) {
        WARN("wallpaper has zero dimension; ignoring\n");
        free(img.pixels);
        free(filebuf);
        wallpaper.loaded = false;
        return -1;
    }

    uint32 *tmp_pixels = malloc(img_pixels);
    if (!tmp_pixels) {
        WARN("malloc for tmp wallpaper pixels failed\n");
        free(img.pixels);
        free(filebuf);
        wallpaper.loaded = false;
        return -1;
    }

    if (img.pixel_format != DM_PIXEL_RGBA32) {
        WARN("wallpaper pixel format unsupported (need RGBA32); got %d\n", img.pixel_format);
        free(img.pixels);
        free(tmp_pixels);
        free(filebuf);
        wallpaper.loaded = false;
        return -1;
    }

    memcpy(tmp_pixels, img.pixels, img_pixels);

    free(img.pixels);
    free(filebuf);

    if ((uint32)img.width == (uint32)FB_W && (uint32)img.height == (uint32)FB_H) {
        wallpaper.pixels = tmp_pixels;
        wallpaper.width = FB_W;
        wallpaper.height = FB_H;
        wallpaper.loaded = true;
    } else {
        uint32 *scaled = malloc((size)FB_W * (size)FB_H * sizeof(uint32));
        if (!scaled) {
            WARN("malloc for scaled wallpaper failed\n");
            free(tmp_pixels);
            wallpaper.loaded = false;
            return -1;
        }
        scale_nearest_neighbour(tmp_pixels, img.width, img.height, scaled, FB_W, FB_H);
        free(tmp_pixels);
        wallpaper.pixels = scaled;
        wallpaper.width = FB_W;
        wallpaper.height = FB_H;
        wallpaper.loaded = true;
    }

    //convert from RGBA32 to ARGB32
    for (int i = 0; i < wallpaper.height * wallpaper.width; i++) {
        uint8 r = (uint8)(wallpaper.pixels[i]);
        uint8 g = (uint8)(wallpaper.pixels[i] >> 8);
        uint8 b = (uint8)(wallpaper.pixels[i] >> 16);
        wallpaper.pixels[i] = FB_RGB(r, g, b);
    }

    INFO("Wallpaper loaded (scaled to %ux%u)\n", wallpaper.width, wallpaper.height);
    return 0;
}

static void free_wallpaper(void) {
    if (wallpaper.pixels) {
        free(wallpaper.pixels);
        wallpaper.pixels = NULL;
    }
    wallpaper.loaded = false;
    wallpaper.width = wallpaper.height = 0;
}

static void render_wallpaper(uint32 *fb_backbuffer) {
    if (!fb_backbuffer) return;

    if (!wallpaper.loaded || !wallpaper.pixels) {
        memset(fb_backbuffer, 0x00, FB_BACKBUFFER_SIZE);
        return;
    }

    memcpy(fb_backbuffer, wallpaper.pixels, FB_BACKBUFFER_SIZE);
}

void render_surfaces(uint32 *fb_backbuffer) {
    render_wallpaper(fb_backbuffer);

    for (int i = 0; i < num_clients; i++) {
        wm_client_t *c = &clients[i];
        if (!c->dirty) continue;
        if (c->status != READY) continue;
        if (!c->surface) {
            WARN("Skipping client idx=%d pid=%u because surface == NULL\n", i, c->pid);
            continue;
        }

        int dst_x0 = max((int)c->x, 0);
        int dst_y0 = max((int)c->y, 0);
        int dst_x1 = min((int)(c->x + c->win_w), FB_W);
        int dst_y1 = min((int)(c->y + c->win_h), FB_H);
        if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) continue;

        int copy_w = dst_x1 - dst_x0;
        int copy_h = dst_y1 - dst_y0;

        int src_x0 = dst_x0 - (int)c->x;
        int src_y0 = dst_y0 - (int)c->y;

        if (src_x0 < 0) src_x0 = 0;
        if (src_y0 < 0) src_y0 = 0;
        if (copy_w > (int)(c->surface_w - src_x0)) copy_w = (int)(c->surface_w - src_x0);
        if (copy_h > (int)(c->surface_h - src_y0)) copy_h = (int)(c->surface_h - src_y0);

        if (copy_w <= 0 || copy_h <= 0) continue;

        INFO("Compositing pid=%u idx=%d dst=(%d,%d) size=(%d,%d) src=(%d,%d)\n",
             c->pid, i, dst_x0, dst_y0, copy_w, copy_h, src_x0, src_y0);

        for (int row = 0; row < copy_h; row++) {
            uint32 *src_row = c->surface + (size)(src_y0 + row) * c->surface_w + src_x0;
            uint32 *dst_row = fb_backbuffer + (size)(dst_y0 + row) * FB_W + dst_x0;
            memcpy(dst_row, src_row, (size)copy_w * sizeof(uint32));
        }

        c->dirty = false;
    }
}

//mouse button flags
#define MOUSE_BTN_LEFT      0x01
#define MOUSE_BTN_RIGHT     0x02
#define MOUSE_BTN_MIDDLE    0x04

//mouse event (pushed by driver)
typedef struct {
    int16 dx; //x movement delta
    int16 dy; //y movement delta
    uint8 buttons; //button state (MOUSE_BTN_*)
    uint8 _pad[3];
} mouse_event_t;

handle_t mouse_h = INVALID_HANDLE;
uint32 mouse_x = FB_W / 2;
uint32 mouse_y = FB_H / 2;
mouse_event_t mprev = {0};

void handle_input() {
    if (focused == -1) return;
    kbd_event_t ev;
    if (kbd_try_read(&ev) == 0) {
        if (focused < 0 || focused >= num_clients) {
            WARN("Got keyboard event but focused index invalid: %d\n", focused);
            return;
        }
        wm_server_msg_t msg = {
            .type = KBD,
            .u.kbd = { .data = ev },
        };
        int rc = channel_send(clients[focused].handle, &msg, sizeof(msg));
        if (rc != 0 && rc != -3) {
            WARN("Failed to forward keyboard to pid=%u idx=%d rc=%d - tearing down\n", clients[focused].pid, focused, rc);
            client_teardown_by_index(focused);
        } else {
            INFO("Forwarded keyboard to pid=%u idx=%d key=%c down=%u\n", clients[focused].pid, focused, ev.codepoint, ev.pressed);
        }
    }
    mouse_event_t m;
    if (mouse_h == INVALID_HANDLE) {
        INFO("Attempting to open mouse handle...\n");
        mouse_h = get_obj(INVALID_HANDLE, "$devices/mouse/channel", RIGHT_READ);
    }
    if (channel_try_recv(mouse_h, &m, sizeof(m)) == sizeof(mouse_event_t)) {
        //first do our things (focus on click, etc)
        mouse_x += m.dx;
        mouse_y += m.dy;

        if (m.buttons & MOUSE_BTN_LEFT && !(mprev.buttons & MOUSE_BTN_LEFT)) {
            for (int i = num_clients - 1; i >= 0; --i) {
                if (clients[i].win_w <= 0 || clients[i].win_h <= 0) continue;
                if (mouse_x >= clients[i].x &&
                    mouse_y >= clients[i].y &&
                    mouse_x <  clients[i].x + clients[i].win_w &&
                    mouse_y <  clients[i].y + clients[i].win_h) {
                    INFO("Focusing on client %u\n", i);
                    focused = i;
                    break;
                }
            }
        }

        mprev = m;
   
        //now we forward if possible
        if (focused < 0 || focused >= num_clients) {
            WARN("Got mouse event but focused index invalid: %d\n", focused);
            return;
        }
        wm_server_msg_t msg = {
            .type = MOUSE,
            .u.mouse = {
                .x = m.dx,
                .y = m.dy,
                .buttons = m.buttons
            }
        };
        int rc = channel_send(clients[focused].handle, &msg, sizeof(msg));
        if (rc != 0 && rc != -3) {
            WARN("Failed to forward mouse to pid=%u idx=%d rc=%d - tearing down\n", clients[focused].pid, focused, rc);
            client_teardown_by_index(focused);
        } else {
            INFO("Forwarded mouse to pid=%u idx=%d dx=%d dy=%d buttons=%u\n", clients[focused].pid, focused, m.dx, m.dy, m.buttons);
        }
    }
}

void render_mouse(uint32 *fb) {
    for (int i = 0; i < cursor_get_width(); i++) {
        for (int j = 0; j < cursor_get_height(); j++) {
            uint32 c = cursor_get_pixel(i, j);
            if (c == 0) continue;
            fb_putpixel(fb, i + mouse_x, j + mouse_y, c);
        }
    }
}

int main(void) {
    handle_t fb_handle = INVALID_HANDLE;
    uint32 *fb_backbuffer = NULL;
    handle_t server_handle = INVALID_HANDLE;
    handle_t client_handle = INVALID_HANDLE;

    fb_setup(&fb_handle, &fb_backbuffer);
    INFO("Setup framebuffer handle %d\n", (int)fb_handle);
    kbd_init();
    INFO("Setup keyboard\n");
    server_setup(&server_handle, &client_handle);
    INFO("Setup server channel\n");

    load_wallpaper();
    render_surfaces(fb_backbuffer);

    while (1) {
        server_listen(&server_handle);
        handle_input();
        render_surfaces(fb_backbuffer);
        render_mouse(fb_backbuffer);

        //present the frame!
        handle_seek(fb_handle, 0, HANDLE_SEEK_SET);
        handle_write(fb_handle, fb_backbuffer, FB_BACKBUFFER_SIZE);

        yield();
    }

    free_wallpaper();
    __builtin_unreachable();
    return 0;
}
