#include <drivers/fb.h>
#include <boot/db.h>
#include <lib/io.h>
#include <mm/mm.h>

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
    
    framebuffer = (uint32 *)P2V(fb->address);
    fb_w = fb->width;
    fb_h = fb->height;
    fb_pitch = fb->pitch;
    
    printf("[fb] initialised: %dx%d@0x%x\n", fb_w, fb_h, fb->address);
}

static void fb_fill_words(uint32 *dest, uint32 color, size count) {
    //if color is uniform (all bytes same) we can use memset
    uint8 b1 = color & 0xFF;
    uint8 b2 = (color >> 8) & 0xFF;
    uint8 b3 = (color >> 16) & 0xFF;
    uint8 b4 = (color >> 24) & 0xFF;
    
    if (b1 == b2 && b2 == b3 && b3 == b4) {
        memset(dest, b1, count * 4);
        return;
    }
    
    while (count--) {
        *dest++ = color;
    }
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
    
    //if pitch matches width * 4 we can clear the whole thing in one memset
    if (fb_pitch == fb_w * 4) {
        fb_fill_words(framebuffer, color, fb_h * fb_w);
    } else {
        for (uint32 y = 0; y < fb_h; y++) {
            uint32 *row = (uint32 *)((uint8 *)framebuffer + y * fb_pitch);
            fb_fill_words(row, color, fb_w);
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
        fb_fill_words(&row[x], color, w);
    }
}

void fb_drawimage(const unsigned char *src, uint32 width, uint32 height, uint32 offset_x, uint32 offset_y) {
    for (uint32 y = 0; y < height; y++) {
        for (uint32 x = 0; x < width; x++) {

            uint8 r = *src++;
            uint8 g = *src++;
            uint8 b = *src++;

            uint32 color = (0xFF << 24) | (r << 16) | (g << 8) | b;

            fb_putpixel(x + offset_x, y + offset_y, color);
        }
    }
}

void fb_scroll(uint32 lines, uint32 bg_color) {
    if (!framebuffer || lines == 0 || lines >= fb_h) return;
    
    //use memmove for the bulk shift
    size copy_size = (fb_h - lines) * fb_pitch;
    memmove(framebuffer, (uint8 *)framebuffer + lines * fb_pitch, copy_size);
    
    //clear the bottom part
    fb_fillrect(0, fb_h - lines, fb_w, lines, bg_color);
}
