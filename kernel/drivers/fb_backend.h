#ifndef DRIVERS_FB_BACKEND_H
#define DRIVERS_FB_BACKEND_H

#include <arch/types.h>

//a display backend, plug any driver in here and the fb_* API will use it
typedef struct fb_backend {
    const char *name;

    //both point at the same buffer for virtio-gpu (host does the blit)
    //for the boot GOP backend draw_buffer is the backbuffer in RAM and
    //display_buffer is VRAM
    void *draw_buffer;
    void *display_buffer;

    uint32 width;
    uint32 height;
    uint32 pitch;      //bytes per row
    size size;       //total bytes (height * pitch)

    //copy draw_buffer to display_buffer for the given rect
    void (*flush_rect)(uint32 x, uint32 y, uint32 w, uint32 h);

    //copy draw_buffer to display_buffer for the whole screen
    void (*flush)(void);

    //optional cleanup (called if the device goes away, not used yet)
    void (*cleanup)(void);
} fb_backend_t;

//register a backend, call this from your driver's BUS-level init
//returns true if the backend was accepted (nothing had claimed it yet)
bool fb_set_backend(fb_backend_t *backend);

//true once a backend has been committed (either by a driver or by
//fb_init_backbuffer falling back to the boot GOP backend)
bool fb_backend_active(void);

#endif
