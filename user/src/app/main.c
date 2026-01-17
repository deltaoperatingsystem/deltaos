#include <system.h>
#include <string.h>
#include <io.h>

typedef struct {
    uint16 width, height;
    char *title;
} window_req_t;

int main(void) {
    //request a window
    handle_t create = get_obj(INVALID_HANDLE, "$gui/window/create", RIGHT_READ | RIGHT_WRITE);
    window_req_t req = { .width = 800, .height = 600, .title = "Test App"};
    int res = channel_send(create, (void*)&req, sizeof(req));

    handle_t window;
    channel_recv(create, &window, sizeof(window));
    if (window == INVALID_HANDLE) return -1;

    //write to the window's buffer
    char path[64];
    size size = req.width * req.height * sizeof(uint32);
    snprintf(path, sizeof(path), "$gui/window/%d/surface", getpid());
    handle_t vmo = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    uint32 *surface = (uint32*)vmo_map(vmo, NULL, 0, size, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    if (!surface) debug_puts("Unable to map surface\n");

    // memset(surface, 0xFF, size);
    channel_send(window, NULL, 0);

    while(1);
    __builtin_unreachable();

    return 0;
}