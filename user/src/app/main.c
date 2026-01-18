#include <system.h>
#include <string.h>
#include <io.h>

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


int main(void) {
    //request a window
    wm_req_t req = (wm_req_t){
        .type = CREATE,
        .u.create.width = 500,
        .u.create.height = 500,
    };
    handle_t wm_handle = get_obj(INVALID_HANDLE, "$gui/wm", RIGHT_READ | RIGHT_WRITE);
    channel_send(wm_handle, &req, sizeof(req));

    wm_res_t res;
    channel_recv(wm_handle, &res, sizeof(res));
    if (res.ack != true) debug_puts("Window creation failed :(\n");

    char path[64];
    snprintf(path, sizeof(path), "$gui/%d/surface", getpid());
    handle_t vmo = get_obj(INVALID_HANDLE, path, RIGHT_MAP | RIGHT_WRITE);
    if (!vmo) debug_puts("Failed to open surface object\n");
    uint32 *surface = vmo_map(vmo, NULL, 0, 500 * 500 * sizeof(uint32), RIGHT_WRITE);
    if (!surface) debug_puts("Failed to map surface\n");

    //test
    int i = 0;
    while (1) {
        i %= 0xFF;
        memset(surface, i++, 500 * 500 * sizeof(uint32));
        req = (wm_req_t){ .type = COMMIT };
        channel_send(wm_handle, &req, sizeof(req));

        //wait for ack
        channel_recv(wm_handle, &res, sizeof(res));
        if (res.ack != true) debug_puts("Failed to commit surface\n");
    }

    return 0;
}