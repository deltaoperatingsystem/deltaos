#include <keyboard.h>
#include <system.h>

static handle_t kbd_channel = INVALID_HANDLE;

int kbd_init(void) {
    if (kbd_channel != INVALID_HANDLE) return 0;  //already initialized
    
    kbd_channel = get_obj(INVALID_HANDLE, "$devices/keyboard/channel", RIGHT_READ | RIGHT_WRITE);
    if (kbd_channel == INVALID_HANDLE) return -1;
    
    //flush stale events from boot
    kbd_flush();
    return 0;
}

int kbd_read(kbd_event_t *event) {
    if (kbd_channel == INVALID_HANDLE) {
        if (kbd_init() < 0) return -1;
    }
    
    int len = channel_recv(kbd_channel, event, sizeof(*event));
    return (len == sizeof(*event)) ? 0 : -1;
}

int kbd_try_read(kbd_event_t *event) {
    if (kbd_channel == INVALID_HANDLE) {
        if (kbd_init() < 0) return -1;
    }
    
    int len = channel_try_recv(kbd_channel, event, sizeof(*event));
    return (len == sizeof(*event)) ? 0 : -1;
}

void kbd_flush(void) {
    if (kbd_channel == INVALID_HANDLE) return;
    
    kbd_event_t discard;
    while (channel_try_recv(kbd_channel, &discard, sizeof(discard)) > 0) {
        //discard
    }
}

void kbd_close(void) {
    if (kbd_channel != INVALID_HANDLE) {
        handle_close(kbd_channel);
        kbd_channel = INVALID_HANDLE;
    }
}

char kbd_getchar(void) {
    kbd_event_t event;
    while (1) {
        if (kbd_read(&event) < 0) return 0;
        if (event.pressed && event.codepoint != 0) {
            return (char)event.codepoint;
        }
    }
}
