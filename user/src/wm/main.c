#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>
#include <keyboard.h>
#include "fb.h"
#include "protocol.h"

#define FB_BACKBUFFER_SIZE  (1280 * 800 * sizeof(uint32))

//cursor functions
extern int cursor_get_width(void);
extern int cursor_get_height(void);
extern uint32 cursor_get_pixel(int x, int y);

//mouse state
static int16 mouse_x = FB_W / 2;
static int16 mouse_y = FB_H / 2;
static handle_t mouse_handle = INVALID_HANDLE;
static bool cursor_moved = true;

static inline int max(int a, int b) {
    return (a > b) ? a : b;
}

static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

void fb_setup(handle_t *h, uint32 **backbuffer) {
    printf("wm: waiting for fb0...\n");
    while ((*h = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_READ | RIGHT_WRITE)) == INVALID_HANDLE) {
        yield();
    }
    printf("wm: got fb0 handle %d\n", *h);
    *backbuffer = malloc(FB_BACKBUFFER_SIZE);
    printf("wm: backbuffer allocated at %p\n", *backbuffer);
    if (*backbuffer == NULL) {
        printf("wm: FATAL: failed to allocate backbuffer!\n");
        exit(1);
    }
}

void server_setup(handle_t *server, handle_t *client) {
    channel_create(server, client);
    ns_register("$gui/wm", *client);
}

void mouse_setup(void) {
    mouse_handle = get_obj(INVALID_HANDLE, "$devices/mouse/channel", RIGHT_READ);
    if (mouse_handle == INVALID_HANDLE) {
        printf("wm: mouse not available\n");
    }
}

void mouse_update(void) {
    if (mouse_handle == INVALID_HANDLE) return;
    
    //mouse_event_t: int16 dx, int16 dy, uint8 buttons, uint8 _pad[3]
    struct {
        int16 dx;
        int16 dy;
        uint8 buttons;
        uint8 _pad[3];
    } event;
    
    while (channel_try_recv(mouse_handle, &event, sizeof(event)) == sizeof(event)) {
        mouse_x += event.dx;
        mouse_y += event.dy;
        
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= FB_W) mouse_x = FB_W - 1;
        if (mouse_y >= FB_H) mouse_y = FB_H - 1;
        
        cursor_moved = true;
    }
}

typedef struct {
    uint32 pid;
    handle_t handle;
    uint32 *surface;
    handle_t vmo;
    uint16 surface_w, surface_h;
    uint16 win_w, win_h;
    uint16 x, y;
    bool dirty;
} wm_client_t;

wm_client_t clients[16];
uint8 num_clients = 0;
int8 focused = -1;

#define WM_ACK  0xFF

void recompute_layout(uint16 screen_w, uint16 screen_h) {
    uint16 tile_h = screen_h / num_clients;
    for (int i = 0; i < num_clients; i++) {
        clients[i].x = 0;
        clients[i].y = i * tile_h;
        clients[i].win_w = screen_w;
        clients[i].win_h = (i == num_clients - 1) ? (screen_h - tile_h * (num_clients - 1)) : tile_h;
        clients[i].dirty = true;
        
        wm_server_msg_t configure = (wm_server_msg_t){
            .type = CONFIGURE,
            .u.configure = {
                .x = 0,
                .y = clients[i].y,
                .w = screen_w,
                .h = clients[i].win_h
            }
        };
        channel_send(clients[i].handle, &configure, sizeof(configure));
    }
}

void window_create(handle_t *server, channel_recv_result_t res, wm_client_msg_t req) {
    if (num_clients == 16) return; //ignore for now

    //create surface
    char path[64];
    handle_t client_vmo = vmo_create(req.u.create.width * req.u.create.height * sizeof(uint32), VMO_FLAG_RESIZABLE, RIGHT_MAP);
    snprintf(path, sizeof(path), "$gui/%d/surface", res.sender_pid);
    ns_register(path, client_vmo);
    uint32 *surface = vmo_map(client_vmo, NULL, 0, req.u.create.width * req.u.create.height * sizeof(uint32), RIGHT_MAP);
    printf("wm: client %d surface mapped at %p (%dx%d)\n", res.sender_pid, surface, req.u.create.width, req.u.create.height);

    //create personal ipc channel
    handle_t wm_end, client_end;
    channel_create(&wm_end, &client_end);
    snprintf(path, sizeof(path), "$gui/%d/channel", res.sender_pid);
    ns_register(path, client_end);

    clients[num_clients++] = (wm_client_t){
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
    };

    wm_server_msg_t resp = (wm_server_msg_t){ .type = ACK, .u.ack = true };
    channel_send(*server, &resp, sizeof(resp));

    recompute_layout(1280, 800);

    if (focused == -1) focused = 0;
}

void window_commit(handle_t client, channel_recv_result_t res, wm_client_msg_t req) {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].pid == res.sender_pid) {
            clients[i].dirty = true;
            wm_server_msg_t resp = (wm_server_msg_t){ .type = ACK, .u.ack = true };
            channel_send(client, &resp, sizeof(resp));
            return;
        }
    }
}

void server_listen(handle_t *server) {
    wm_client_msg_t msg;
    channel_recv_result_t res;
    if (channel_try_recv_msg(*server, &msg, sizeof(wm_client_msg_t), NULL, 0, &res) == 0) {
        switch (msg.type) {
            case CREATE: window_create(server, res, msg); break;
        }
    }

    for (int i = 0; i < num_clients; i++) {
        if (channel_try_recv_msg(clients[i].handle, &msg, sizeof(wm_client_msg_t), NULL, 0, &res) != 0) continue;

        switch (msg.type) {
            case COMMIT: window_commit(clients[i].handle, res, msg); break;
            case RESIZE: {
                vmo_unmap(clients[i].surface, clients[i].surface_w * clients[i].surface_h * sizeof(uint32));
                clients[i].surface_w = msg.u.resize.width;
                clients[i].surface_h = msg.u.resize.height;
                clients[i].surface = vmo_map(clients[i].vmo, NULL, 0, clients[i].surface_w * clients[i].surface_h * sizeof(uint32), RIGHT_MAP);
                clients[i].dirty = true;
                break;
            }
        }
    }
}

void render_surfaces(handle_t fb_handle, uint32 *fb_backbuffer) {
    static bool bg_dirty = true;

    for (int i = 0; i < num_clients; i++) {
        if (clients[i].dirty == false) continue;
        wm_client_t c = clients[i];

        //visible copy rectangle in fb space
        int dst_x0 = max(c.x, 0);
        int dst_y0 = max(c.y, 0);
        int dst_x1 = min(c.x + c.win_w, FB_W);
        int dst_y1 = min(c.y + c.win_h, FB_H);
        if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) continue; //nothing visible, skip

        //how many pixels to copy
        int copy_w = dst_x1 - dst_x0;
        int copy_h = dst_y1 - dst_y0;

        //where in the surface to start
        int src_x0 = dst_x0 - c.x;
        int src_y0 = dst_y0 - c.y;

        //clamp
        src_x0 = max(src_x0, 0);
        src_y0 = max(src_y0, 0);
        copy_w = min(copy_w, c.surface_w - src_x0);
        copy_h = min(copy_h, c.surface_h - src_y0);

        if (copy_w <= 0 || copy_h <= 0) continue; //nothing to copy, skip

        for (int row = 0; row < copy_h; row++) {
            uint32 *src_row = c.surface + (src_y0 + row) * c.surface_w + src_x0;
            uint32 *dst_row = fb_backbuffer + (dst_y0 + row) * FB_W + dst_x0;
            if (!dst_row || !src_row) {
                printf("wm: ERROR: NULL ptr in memcpy client %d, row %d, dst %p, src %p, fb_bb %p, surface %p\n", 
                        i, row, dst_row, src_row, fb_backbuffer, c.surface);
                exit(1);
            }
            memcpy(dst_row, src_row, copy_w * sizeof(uint32));
        }
        
        clients[i].dirty = false;
    }
    //draw cursor
    if (cursor_moved) {
        for (int cy = 0; cy < cursor_get_height(); cy++) {
            for (int cx = 0; cx < cursor_get_width(); cx++) {
                uint32 pixel = cursor_get_pixel(cx, cy);
                if (pixel) {
                    int sx = mouse_x + cx;
                    int sy = mouse_y + cy;
                    if (sx >= 0 && sx < FB_W && sy >= 0 && sy < FB_H) {
                        fb_backbuffer[sy * FB_W + sx] = pixel;
                    }
                }
            }
        }
        cursor_moved = false;
    }
    
    handle_seek(fb_handle, 0, HANDLE_SEEK_SET);
    handle_write(fb_handle, fb_backbuffer, FB_BACKBUFFER_SIZE);
}

void handle_input() {
    if (focused == -1) return;
    kbd_event_t ev;
    if (kbd_try_read(&ev) == 0) {
        wm_server_msg_t msg = {
            .type = KBD,
            .u.kbd.data = ev,
        };
        channel_send(clients[focused].handle, &msg, sizeof(msg));
    }
}

int main(void) {
    handle_t fb_handle = INVALID_HANDLE;
    uint32 *fb_backbuffer = NULL;
    handle_t server_handle = INVALID_HANDLE;
    handle_t client_handle = INVALID_HANDLE;

    fb_setup(&fb_handle, &fb_backbuffer);
    kbd_init();
    mouse_setup();
    server_setup(&server_handle, &client_handle);

    while (1) {
        server_listen(&server_handle);
        handle_input();
        mouse_update();
        render_surfaces(fb_handle, fb_backbuffer);

        yield();
    }
    __builtin_unreachable();
    return 0;
}
