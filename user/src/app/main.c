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
    if (res.ack != true) {
        debug_puts("Window creation failed :(\n");
    } else {
        debug_puts("Created a window successfully\n");
    }

    while(1);
    __builtin_unreachable();

    return 0;
}