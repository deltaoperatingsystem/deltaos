#include <system.h>
#include <string.h>
#include <io.h>
#include "../wm/protocol.h"

int main(void) {
    uint16 w = 500, h = 500;
    //request a window
    wm_client_msg_t req = (wm_client_msg_t){
        .type = CREATE,
        .u.create.width = w,
        .u.create.height = h,
    };
    handle_t wm_handle = get_obj(INVALID_HANDLE, "$gui/wm", RIGHT_READ | RIGHT_WRITE);
    channel_send(wm_handle, &req, sizeof(req));

    wm_server_msg_t res;
    channel_recv(wm_handle, &res, sizeof(res));

    char path[64];
    snprintf(path, sizeof(path), "$gui/%d/surface", getpid());
    handle_t vmo = INVALID_HANDLE;
    while ((vmo = get_obj(INVALID_HANDLE, path, RIGHT_MAP | RIGHT_WRITE)) == INVALID_HANDLE) {
        yield();
    }
    uint32 *surface = vmo_map(vmo, NULL, 0, w * h * sizeof(uint32), RIGHT_WRITE | RIGHT_MAP);
    if (!surface) {
        printf("app: FATAL: failed to map surface!\n");
        exit(1);
    }

    snprintf(path, sizeof(path), "$gui/%d/channel", getpid());
    handle_t channel = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_WRITE);

    //test
    int i = 0;
    while (1) {
        if (channel_recv(channel, &res, sizeof(res)) > 0) {
            switch(res.type) {
                case CONFIGURE: {
                    if (res.u.configure.w <= w && res.u.configure.h <= h) break;
                    vmo_unmap(surface, w * h * sizeof(uint32));
                    w = res.u.configure.w;
                    h = res.u.configure.h;
                    vmo_resize(vmo, w * h * sizeof(uint32));
                    surface = vmo_map(vmo, NULL, 0, w * h * sizeof(uint32), RIGHT_WRITE | RIGHT_MAP);
                    req = (wm_client_msg_t){ .type = RESIZE, .u.resize.width = w, .u.resize.height = h };
                    channel_send(channel, &req, sizeof(req));
                    break;
                }
                case KBD: {
                    dprintf("%c\n", res.u.kbd.data.codepoint);
                    break;
                }
            }
        }

        i %= 0xFF;
        memset(surface, i++, w * h * sizeof(uint32));
        req = (wm_client_msg_t){ .type = COMMIT };
        channel_send(channel, &req, sizeof(req));

        yield();
    }

    return 0;
}