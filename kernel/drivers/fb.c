#include <drivers/fb.h>
#include <fs/fs.h>
#include <boot/db.h>
#include <lib/io.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <arch/mmu.h>
#include <drivers/init.h>

static uint32 *framebuffer = NULL;  //VRAM (slow, uncached)
static uint32 *backbuffer = NULL;   //RAM (fast, cached)
static uint32 fb_w = 0;
static uint32 fb_h = 0;
static uint32 fb_pitch = 0;
static size fb_size = 0;

static void fb_fill_words_target(uint32 *dest, uint32 color, size count) {
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

static void fb_copy_rect_target(uint32 *target, uint32 dst_x, uint32 dst_y, uint32 src_x, uint32 src_y, uint32 w, uint32 h);

static void fb_fillrect_target(uint32 *target, uint32 x, uint32 y, uint32 w, uint32 h, uint32 color) {
    if (!target) return;
    
    //clamp to screen bounds
    if (x >= fb_w || y >= fb_h) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    
    for (uint32 py = y; py < y + h; py++) {
        uint32 *row = (uint32 *)((uint8 *)target + py * fb_pitch);
        fb_fill_words_target(&row[x], color, w);
    }
}

static void fb_scroll_target(uint32 *target, uint32 lines, uint32 bg_color) {
    if (!target || lines == 0 || lines >= fb_h) return;
    
    fb_copy_rect_target(target, 0, 0, 0, lines, fb_w, fb_h - lines);
    fb_fillrect_target(target, 0, fb_h - lines, fb_w, lines, bg_color);
}

static ssize fb_obj_read(object_t *obj, void *buf, size len, size offset) {
    (void)obj;
    
    //bounds check
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size) len = fb_size - offset;
    
    //read from frontbuffer (VRAM)
    memcpy(buf, (uint8*)framebuffer + offset, len);
    return len;
}

static ssize fb_obj_write(object_t *obj, const void *buf, size len, size offset) {
    (void)obj;
    
    //bounds check
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size) len = fb_size - offset;
    
    //write to backbuffer/frontbuffer
    uint8 *target = backbuffer ? (uint8*)backbuffer : (uint8*)framebuffer;
    
    memcpy(target + offset, buf, len);
    
    //auto-flip when writing from start (full frame write)
    if (offset == 0 && backbuffer) fb_flip();
    
    return len;
}

static int fb_obj_stat(object_t *obj, stat_t *st) {
    (void)obj;
    if (!st) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DEVICE;
    st->size = fb_size;
    st->width = fb_w;
    st->height = fb_h;
    st->pitch = fb_pitch;
    st->ctime = st->mtime = st->atime = 0;
    return 0;
}

static object_ops_t fb_object_ops = {
    .read = fb_obj_read,
    .write = fb_obj_write,
    .stat = fb_obj_stat
};

static object_t *fb_object = NULL;

void fb_init(void) {
    if (framebuffer) return;

    struct db_tag_framebuffer *fb = db_get_framebuffer();
    
    if (!fb) {
        puts("[fb] no framebuffer available\n");
        return;
    }
    
    framebuffer = (uint32 *)P2V(fb->address);
    fb_w = fb->width;
    fb_h = fb->height;
    fb_pitch = fb->pitch;
    fb_size = fb_h * fb_pitch;

    //remap VRAM as write-combining (huge performance boost on real hardware)
    mmu_map_range(mmu_get_kernel_pagemap(), (uintptr)framebuffer, fb->address, 
                  (fb_size + 0xFFF) / 0x1000, 
                  MMU_FLAG_WRITE | MMU_FLAG_WC);

    fb_object = object_create(OBJECT_DEVICE, &fb_object_ops, NULL);
    if (fb_object) ns_register("$devices/fb0", fb_object);
    
    printf("[fb] initialised: %dx%d@0x%X (WC mapped)\n", fb_w, fb_h, fb->address);
}

void fb_init_backbuffer(void) {
    if (!framebuffer) return;
    if (backbuffer) return;

    backbuffer = kmalloc(fb_size);
    
    if (!backbuffer) {
        printf("[fb] WARN: failed to allocate %zu byte backbuffer, using direct VRAM\n", fb_size);
        return;
    }
    
    //preserve whatever was already on screen, this is only for if you
    //do any early-boot logging
    memcpy(backbuffer, framebuffer, fb_size);
    printf("[fb] backbuffer allocated: %zu bytes\n", fb_size);
}

DECLARE_DRIVER(fb_init, INIT_LEVEL_BUS);
DECLARE_DRIVER(fb_init_backbuffer, INIT_LEVEL_DEVICE);

//get the active buffer (backbuffer if available else VRAM)
static inline uint32 *get_draw_target(void) {
    return backbuffer ? backbuffer : framebuffer;
}

void fb_flip(void) {
    if (!backbuffer || !framebuffer) return;
    memcpy(framebuffer, backbuffer, fb_size);
}

void fb_flip_rect(uint32 x, uint32 y, uint32 w, uint32 h) {
    if (!backbuffer || !framebuffer) return;
    if (x >= fb_w || y >= fb_h || w == 0 || h == 0) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;

    size row_bytes = w * 4;
    for (uint32 py = 0; py < h; py++) {
        uint8 *src = (uint8 *)backbuffer + (y + py) * fb_pitch + x * 4;
        uint8 *dst = (uint8 *)framebuffer + (y + py) * fb_pitch + x * 4;
        memcpy(dst, src, row_bytes);
    }
}

static void fb_copy_rect_target(uint32 *target, uint32 dst_x, uint32 dst_y, uint32 src_x, uint32 src_y, uint32 w, uint32 h) {
    if (!target) return;
    if (w == 0 || h == 0) return;
    if (dst_x >= fb_w || dst_y >= fb_h || src_x >= fb_w || src_y >= fb_h) return;

    if (src_x + w > fb_w) w = fb_w - src_x;
    if (dst_x + w > fb_w) w = fb_w - dst_x;
    if (src_y + h > fb_h) h = fb_h - src_y;
    if (dst_y + h > fb_h) h = fb_h - dst_y;

    size row_bytes = w * 4;
    bool reverse = (dst_y > src_y);

    if (reverse) {
        for (int32 py = (int32)h - 1; py >= 0; py--) {
            uint8 *src = (uint8 *)target + (src_y + (uint32)py) * fb_pitch + src_x * 4;
            uint8 *dst = (uint8 *)target + (dst_y + (uint32)py) * fb_pitch + dst_x * 4;
            memmove(dst, src, row_bytes);
        }
    } else {
        for (uint32 py = 0; py < h; py++) {
            uint8 *src = (uint8 *)target + (src_y + py) * fb_pitch + src_x * 4;
            uint8 *dst = (uint8 *)target + (dst_y + py) * fb_pitch + dst_x * 4;
            memmove(dst, src, row_bytes);
        }
    }
}

void fb_copy_rect(uint32 dst_x, uint32 dst_y, uint32 src_x, uint32 src_y, uint32 w, uint32 h) {
    uint32 *target = get_draw_target();
    fb_copy_rect_target(target, dst_x, dst_y, src_x, src_y, w, h);
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
    uint32 *target = get_draw_target();
    if (!target) return;
    
    if (fb_pitch == fb_w * 4) {
        fb_fill_words_target(target, color, fb_h * fb_w);
    } else {
        for (uint32 y = 0; y < fb_h; y++) {
            uint32 *row = (uint32 *)((uint8 *)target + y * fb_pitch);
            fb_fill_words_target(row, color, fb_w);
        }
    }
}

void fb_putpixel(uint32 x, uint32 y, uint32 color) {
    uint32 *target = get_draw_target();
    if (!target || x >= fb_w || y >= fb_h) return;
    
    uint32 *row = (uint32 *)((uint8 *)target + y * fb_pitch);
    row[x] = color;
}

void fb_fillrect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color) {
    uint32 *target = get_draw_target();
    fb_fillrect_target(target, x, y, w, h, color);
}

void fb_drawimage(const unsigned char *src, uint32 width, uint32 height, uint32 offset_x, uint32 offset_y) {
    uint32 *target = get_draw_target();
    if (!target || !src) return;
    if (offset_x >= fb_w || offset_y >= fb_h) return;
    if (offset_x + width > fb_w) width = fb_w - offset_x;
    if (offset_y + height > fb_h) height = fb_h - offset_y;

    for (uint32 y = 0; y < height; y++) {
        uint32 *row = (uint32 *)((uint8 *)target + (offset_y + y) * fb_pitch) + offset_x;
        for (uint32 x = 0; x < width; x++) {
            uint8 r = *src++;
            uint8 g = *src++;
            uint8 b = *src++;
            row[x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

void fb_drawglyph(uint32 x, uint32 y, const uint8 *glyph, uint32 fg, uint32 bg, uint32 width, uint32 height) {
    uint32 *target = get_draw_target();
    if (!target || !glyph) return;
    if (x >= fb_w || y >= fb_h) return;
    if (x + width > fb_w) width = fb_w - x;
    if (y + height > fb_h) height = fb_h - y;

    for (uint32 py = 0; py < height; py++) {
        uint8 row_bits = glyph[py];
        uint32 *row = (uint32 *)((uint8 *)target + (y + py) * fb_pitch) + x;
        for (uint32 px = 0; px < width; px++) {
            row[px] = (row_bits & (0x80 >> px)) ? fg : bg;
        }
    }
}

void fb_scroll(uint32 lines, uint32 bg_color) {
    if (lines == 0 || lines >= fb_h) return;

    if (backbuffer && framebuffer) {
        fb_scroll_target(backbuffer, lines, bg_color);
        fb_scroll_target(framebuffer, lines, bg_color);
        return;
    }

    fb_scroll_target(get_draw_target(), lines, bg_color);
}

// uses Bresenham's algorithm
void fb_drawline(uint32 x1, uint32 y1, uint32 x2, uint32 y2, uint32 colour) {
    int dx = x2 - x1; if (dx < 0) dx = -dx;
    int dy = y2 - y1; if (dy < 0) dy = -dy;
    
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        fb_putpixel(x1, y1, colour);

        if (x1 == x2 && y1 == y2) break;

        int e2 = 2 * err;

        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }

        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}
