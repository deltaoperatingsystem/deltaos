#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>

#define FB_BACKBUFFER_SIZE  (1280 * 800 * sizeof(uint32))

handle_t fb_handle = INVALID_HANDLE;
uint32 *fb_backbuffer = NULL;

void fb_setup(handle_t *h, uint32 **backbuffer) {
    *h = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_READ | RIGHT_WRITE);
    *backbuffer = malloc(FB_BACKBUFFER_SIZE);
}

void kbd_setup(handle_t *h) {
    *h = get_obj(INVALID_HANDLE, "$devices/keyboard", RIGHT_READ);
}

void server_setup(handle_t *server, handle_t *client) {
    channel_create(server, client);
    ns_register("$gui/wm", *client);
}

typedef struct {
    enum {
        CREATE, COMMIT, DESTROY,
    } type;
    union {
        struct {
            uint16 width, height;
        } create;
        struct {

        } commit;
        struct {

        } destroy;
    } u;
} wm_req_t;

typedef struct {
    bool ack;
    union {
        struct {

        } create;
        struct {

        } commit;
        struct {

        } destroy;
    } u;
} wm_res_t;

typedef struct {
    uint32 pid;
    uint32 *surface;
    handle_t vmo_handle;
    uint16 width, height;
    bool dirty;
} wm_client_t;

wm_client_t clients[16];
uint8 num_clients = 0;

#define WM_ACK  0xFF

void window_create(handle_t *server, channel_recv_result_t res, wm_req_t req) {
    if (num_clients == 16) return; //ignore for now

    //create surface
    char path[64];
    handle_t client_vmo = vmo_create(req.u.create.width * req.u.create.height * sizeof(uint32), VMO_FLAG_NONE, RIGHT_MAP | RIGHT_WRITE);
    snprintf(path, sizeof(path), "$gui/%d/surface", res.sender_pid);
    ns_register(path, client_vmo);
    uint32 *surface = vmo_map(client_vmo, NULL, 0, req.u.create.width * req.u.create.height * sizeof(uint32), RIGHT_WRITE | RIGHT_MAP);
    if (!surface) debug_puts("wm: failed to map client's surface\n");

    clients[num_clients++] = (wm_client_t){
        .pid = res.sender_pid,
        .surface = surface,
        .vmo_handle = client_vmo,
        .width = req.u.create.width,
        .height = req.u.create.height,
        .dirty = false,
    };

    wm_res_t resp = (wm_res_t){ .ack = true };
    channel_send(*server, &resp, sizeof(resp));
}

#include "fb.h"
void window_commit(handle_t *server, channel_recv_result_t res, wm_req_t req) {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].pid == res.sender_pid) {
            clients[i].dirty = true;
            
            wm_res_t resp = (wm_res_t){ .ack = true };
            channel_send(*server, &resp, sizeof(resp));
            return;
        }
    }
}

void server_listen(handle_t *server) {
    wm_req_t msg;
    channel_recv_result_t res;
    if (channel_recv_msg(*server, &msg, sizeof(wm_req_t), NULL, 0, &res) != 0) {
        yield(); return;
    }

    switch (msg.type) {
        case CREATE: window_create(server, res, msg); break;
        case COMMIT: window_commit(server, res, msg); break;
    }
}

void render_surfaces() {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].dirty == false) continue;
        fb_drawimage(fb_backbuffer, (char*)clients[i].surface, 0, 0, clients[i].width, clients[i].height);
        clients[i].dirty = false;
    }
    handle_seek(fb_handle, 0, HANDLE_SEEK_SET);
    handle_write(fb_handle, fb_backbuffer, FB_BACKBUFFER_SIZE);
}

int main(void) {
    handle_t kbd_handle = INVALID_HANDLE;
    handle_t server_handle = INVALID_HANDLE;
    handle_t client_handle = INVALID_HANDLE;

    fb_setup(&fb_handle, &fb_backbuffer);
    kbd_setup(&kbd_handle);
    server_setup(&server_handle, &client_handle);

    spawn("$files/initrd/app", 0, NULL);
    while (1) {
        server_listen(&server_handle);
        // handle_input();
        render_surfaces();
    }
    __builtin_unreachable();
    return 0;
}
