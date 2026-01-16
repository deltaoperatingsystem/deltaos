#include <io.h>
#include <system.h>
#include <memory.h>

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

// uint32 buffer[1280][800] = {0};
// uint32 x = 0, y = 0;

int main(void) {
    int32 mouse = get_obj(INVALID_HANDLE, "$devices/mouse/channel", RIGHT_READ);
    int32 fb = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_WRITE);
    while (1) {
        mouse_event_t event;
        channel_recv(mouse, &event, sizeof(event));
        if (!event.dx ||!event.dy) continue;
        printf("%x %x\n", event.dx, event.dy);
        // buffer[x][y] = 0x0;
        // x += event.dx; y += event.dy;
        // buffer[x][y] = 0xFFFFFFFF;
        // handle_write(fb, buffer, sizeof(buffer));
    }
    return 0;
}