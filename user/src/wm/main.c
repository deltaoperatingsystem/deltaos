#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>
#include "fb.h"

//mouse button flags
#define MOUSE_BTN_LEFT      0x01
#define MOUSE_BTN_RIGHT     0x02
#define MOUSE_BTN_MIDDLE    0x04

//mouse event structure
typedef struct {
    int16 dx; //x movement delta
    int16 dy; //y movement delta
    uint8 buttons; //button state (MOUSE_BTN_*)
    uint8 _pad[3];
} mouse_event_t;

uint32 *fb = NULL;
handle_t fb_handle = INVALID_HANDLE;

uint32 x = FB_W / 2;
uint32 y = FB_H / 2;

int main(int argc, char *argv[]) {
    fb = malloc(FB_W * FB_H * sizeof(uint32));
    fb_handle = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_WRITE);
    int32 mouse_channel = get_obj(INVALID_HANDLE, "$devices/mouse/channel", RIGHT_READ);
    if (mouse_channel == INVALID_HANDLE) {
        puts("Failed to open $devices/mouse/channel\n");
        return 1;
    }    
    while (1) {
        mouse_event_t event;
        int len = channel_recv(mouse_channel, &event, sizeof(event));
        
        if (len <= 0) {
            yield();
            continue;
        }
        
        if (len == sizeof(mouse_event_t)) {
            extern const struct { unsigned int width; unsigned int height; unsigned int bytes_per_pixel; char *comment; unsigned char pixel_data[12 * 19 * 4 + 1]; }
            cursor_sprite;
            fb_fillrect(fb, x, y, cursor_sprite.width, cursor_sprite.height, 0);
            x += event.dx;
            y += event.dy;
            
            if (x > FB_W) x = FB_W - 1;
            if (x < 0) x = 0;
            if (y > FB_H) y = FB_H - 1;
            if (y < 0) y = 0;

            fb_drawimage(fb, cursor_sprite.pixel_data, x, y, cursor_sprite.width, cursor_sprite.height);
            handle_write(fb_handle, fb, 4096000);
            handle_seek(fb_handle, 0, HANDLE_SEEK_SET);
        } else {
            printf("Received unexpected message size: %d\n", len);
        }
    }
    
    return 0;
}