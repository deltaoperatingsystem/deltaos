#ifndef DRIVERS_FB_H
#define DRIVERS_FB_H

#include <arch/types.h>

//initialize framebuffer from boot info
void fb_init(void);

//allocate backbuffer for double buffering
void fb_init_backbuffer(void);

//copy backbuffer to VRAM
void fb_flip(void);

//check if framebuffer is available
bool fb_available(void);

//get framebuffer dimensions
uint32 fb_width(void);
uint32 fb_height(void);

//basic drawing operations
void fb_clear(uint32 color);
void fb_putpixel(uint32 x, uint32 y, uint32 color);
void fb_fillrect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color);

// image rendering!
void fb_drawimage(const unsigned char *src, uint32 width, uint32 height, uint32 x, uint32 y);

void fb_scroll(uint32 lines, uint32 bg_color);

//color helpers (assumes BGRX format from UEFI GOP)
#define FB_RGB(r, g, b) ((uint32)((b) | ((g) << 8) | ((r) << 16)))

#define FB_BLACK   FB_RGB(0, 0, 0)
#define FB_WHITE   FB_RGB(255, 255, 255)
#define FB_RED     FB_RGB(255, 0, 0)
#define FB_GREEN   FB_RGB(0, 255, 0)
#define FB_BLUE    FB_RGB(0, 0, 255)
#define FB_GRAY    FB_RGB(128, 128, 128)

#endif
