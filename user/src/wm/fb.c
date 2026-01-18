#include <types.h>
#include "fb.h"

void fb_putpixel(uint32 *fb, uint32 x, uint32 y, uint32 colour) {
    if (x >= FB_W || y >= FB_H) return;
    fb[y * FB_W + x] = colour;
}

void fb_fillrect(uint32 *fb, uint32 x, uint32 y, uint32 w, uint32 h, uint32 colour) {
    if (!fb) return;
    
    //clamp to screen bounds
    if (x >= FB_W || y >= FB_H) return;
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            fb_putpixel(fb, x + i, y + j, colour);
        }
    }
}

void fb_drawimage(uint32 *fb, const unsigned char *src, uint32 x, uint32 y, uint32 w, uint32 h) {
    for (uint32 i = 0; i < h; i++) {
        for (uint32 j = 0; j < w; j++) {
            uint8 r = *src++;
            uint8 g = *src++;
            uint8 b = *src++;
            src++; // ignore extra A channel

            uint32 color = FB_RGB(r, g, b);

            fb_putpixel(fb, j + x, i + y, color);
        }
    }
}