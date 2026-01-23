#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>
#include "fb.h"
#include "protocol.h"
#include "../../libkeyboard/include/keyboard.h"

#define FB_BACKBUFFER_SIZE  (1280 * 800 * sizeof(uint32))

/**
 * Selects the greater of two integers.
 * @returns The greater of `a` and `b`.
 */
static inline int max(int a, int b) {
    return (a > b) ? a : b;
}

/**
 * Selects the smaller of two integers.
 *
 * @returns The smaller of `a` and `b`; if they are equal, returns that value.
 */
static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

/**
 * Obtain a handle to the primary framebuffer device and allocate a software backbuffer.
 *
 * @param h Pointer that will receive the framebuffer device handle (opened with read and write rights).
 * @param backbuffer Pointer that will receive a newly allocated buffer of size FB_BACKBUFFER_SIZE for composing framebuffer contents.
 */
void fb_setup(handle_t *h, uint32 **backbuffer) {
    *h = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_READ | RIGHT_WRITE);
    *backbuffer = malloc(FB_BACKBUFFER_SIZE);
}

/**
 * Create a new IPC channel pair and publish the client endpoint.
 *
 * Creates a channel pair and stores the server endpoint in `*server`.
 * The client endpoint stored in `*client` is registered in the namespace
 * under "$gui/wm" so external processes can connect to the window manager.
 *
 * @param server Pointer to receive the server-side channel handle.
 * @param client Pointer to receive the client-side channel handle (registered as "$gui/wm").
 */
void server_setup(handle_t *server, handle_t *client) {
    channel_create(server, client);
    ns_register("$gui/wm", *client);
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

/**
 * Register a newly connecting client: create and map its surface VMO, create a private IPC
 * channel, add the client to the manager's client list, acknowledge the creator, and recompute layout.
 *
 * If the maximum client capacity (16) has been reached, the request is ignored.
 *
 * @param server Pointer to the server channel handle used to send the ACK response.
 * @param res IPC receive result containing the sender PID of the requesting client.
 * @param req Client request message containing the surface dimensions for creation.
 *
 * Side effects:
 *  - Allocates and maps a VMO for the client's surface and registers it under "$gui/<pid>/surface".
 *  - Creates a private channel registered under "$gui/<pid>/channel" and stores the server end.
 *  - Appends a new wm_client_t to the global clients array and marks it for layout.
 *  - Sends an ACK back to the caller and invokes recompute_layout.
 *  - If no client is currently focused, sets focus to the newly added client.
 */
void window_create(handle_t *server, channel_recv_result_t res, wm_client_msg_t req) {
    if (num_clients == 16) return; //ignore for now

    //create surface
    char path[64];
    handle_t client_vmo = vmo_create(req.u.create.width * req.u.create.height * sizeof(uint32), VMO_FLAG_RESIZABLE, RIGHT_MAP);
    snprintf(path, sizeof(path), "$gui/%d/surface", res.sender_pid);
    ns_register(path, client_vmo);
    uint32 *surface = vmo_map(client_vmo, NULL, 0, req.u.create.width * req.u.create.height * sizeof(uint32), RIGHT_MAP);

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

void window_commit(handle_t client, channel_recv_result_t res) {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].pid == res.sender_pid) {
            clients[i].dirty = true;
            wm_server_msg_t resp = (wm_server_msg_t){ .type = ACK, .u.ack = true };
            channel_send(client, &resp, sizeof(resp));
            return;
        }
    }
}

/**
 * Polls the server channel and all connected client channels for pending window manager messages
 * and processes them non-blockingly.
 *
 * Listens for CREATE messages on the server channel to accept new clients. For each connected
 * client, processes COMMIT messages to mark a client's buffer update and RESIZE messages to
 * resize, remap, and mark the client's surface as dirty. Unknown message types are ignored.
 *
 * @param server Pointer to the server channel handle used to accept new clients and receive messages.
 */
void server_listen(handle_t *server) {
    wm_client_msg_t msg;
    channel_recv_result_t res;
    if (channel_try_recv_msg(*server, &msg, sizeof(wm_client_msg_t), NULL, 0, &res) == 0) {
        switch (msg.type) {
            case CREATE: window_create(server, res, msg); break;
            default: break;
        }
    }

    for (int i = 0; i < num_clients; i++) {
        if (channel_try_recv_msg(clients[i].handle, &msg, sizeof(wm_client_msg_t), NULL, 0, &res) != 0) continue;

        switch (msg.type) {
            case COMMIT: window_commit(clients[i].handle, res); break;
            case RESIZE: {
                vmo_unmap(clients[i].surface, clients[i].surface_w * clients[i].surface_h * sizeof(uint32));
                clients[i].surface_w = msg.u.resize.width;
                clients[i].surface_h = msg.u.resize.height;
                clients[i].surface = vmo_map(clients[i].vmo, NULL, 0, clients[i].surface_w * clients[i].surface_h * sizeof(uint32), RIGHT_MAP);
                clients[i].dirty = true;
                break;
            }
            default: break;
        }
    }
}

/**
 * Composite dirty client surfaces into the framebuffer backbuffer and flush it to the framebuffer device.
 *
 * For each client marked dirty, copies the visible portion of that client's surface into the provided
 * framebuffer backbuffer (clamped to framebuffer and surface bounds), clears the client's dirty flag,
 * then writes the entire backbuffer to the framebuffer starting at offset zero.
 *
 * @param fb_handle Handle to the framebuffer device to write the backbuffer to.
 * @param fb_backbuffer Pointer to a pixel buffer of size FB_BACKBUFFER_SIZE representing the framebuffer backbuffer.
 */
void render_surfaces(handle_t fb_handle, uint32 *fb_backbuffer) {
    if (num_clients < 1) return;
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
            memcpy(dst_row, src_row, copy_w * sizeof(uint32));
        }
        
        clients[i].dirty = false;
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

/**
 * Program entry point; initializes subsystems and runs the window manager main loop.
 *
 * Initializes the framebuffer, keyboard, and server/channel infrastructure, then
 * enters the primary event loop that accepts client connections and messages,
 * forwards input to the focused client, and renders client surfaces to the
 * framebuffer backbuffer.
 *
 * @returns 0 on normal termination (function is intended to run indefinitely; return is unreachable under normal operation).
 */
int main(void) {
    handle_t fb_handle = INVALID_HANDLE;
    uint32 *fb_backbuffer = NULL;
    handle_t server_handle = INVALID_HANDLE;
    handle_t client_handle = INVALID_HANDLE;

    fb_setup(&fb_handle, &fb_backbuffer);
    kbd_init();
    server_setup(&server_handle, &client_handle);

    while (1) {
        server_listen(&server_handle);
        handle_input();
        render_surfaces(fb_handle, fb_backbuffer);

        yield();
    }
    __builtin_unreachable();
    return 0;
}