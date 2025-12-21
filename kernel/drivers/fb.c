#include <drivers/fb.h>
#include <boot/db.h>
#include <lib/io.h>

static uint32 *framebuffer = NULL;
static uint32 fb_w = 0;
static uint32 fb_h = 0;
static uint32 fb_pitch = 0;

void fb_init(void) {
    struct db_tag_framebuffer *fb = db_get_framebuffer();
    
    if (!fb) {
        puts("[fb] no framebuffer available\n");
        return;
    }
    
    framebuffer = (uint32 *)(uintptr)fb->address;
    fb_w = fb->width;
    fb_h = fb->height;
    fb_pitch = fb->pitch;
    
    printf("[fb] initialised: %dx%d@0x%x\n", fb_w, fb_h, fb->address);
}

bool fb_available(void) {
    return framebuffer != NULL;
}

uint32 fb_width(void) {
    return fb_w;
}

uint32 fb_height(void) {
    return fb_h;
}

void fb_clear(uint32 color) {
    if (!framebuffer) return;
    
    for (uint32 y = 0; y < fb_h; y++) {
        uint32 *row = (uint32 *)((uint8 *)framebuffer + y * fb_pitch);
        for (uint32 x = 0; x < fb_w; x++) {
            row[x] = color;
        }
    }
}

void fb_putpixel(uint32 x, uint32 y, uint32 color) {
    if (!framebuffer || x >= fb_w || y >= fb_h) return;
    
    uint32 *row = (uint32 *)((uint8 *)framebuffer + y * fb_pitch);
    row[x] = color;
}

void fb_fillrect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color) {
    if (!framebuffer) return;
    
    //clamp to screen bounds
    if (x >= fb_w || y >= fb_h) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    
    for (uint32 py = y; py < y + h; py++) {
        uint32 *row = (uint32 *)((uint8 *)framebuffer + py * fb_pitch);
        for (uint32 px = x; px < x + w; px++) {
            row[px] = color;
        }
    }
}
