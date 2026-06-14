#include <drivers/fb.h>
#include <drivers/fb_backend.h>
#include <fs/fs.h>
#include <boot/db.h>
#include <lib/io.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <arch/mmu.h>
#include <drivers/init.h>

//the active backend, whoever calls fb_set_backend() first wins
//NULL until a driver registers or fb_init_backbuffer() falls back to boot-GOP
//drawing still works before this is set: helpers fall back to boot_vram directly
static fb_backend_t *active_backend = NULL;

//boot GOP backend (fallback when no GPU driver grabs the display)
static uint32 *boot_vram = NULL;  //VRAM (slow, uncached)
static uint32 *boot_bb = NULL;   //RAM (fast, cached)
static uint32 boot_w = 0;
static uint32 boot_h = 0;
static uint32 boot_pitch = 0;
static size boot_size = 0;

static fb_backend_t boot_backend;

static void boot_flush(void) {
    if (!boot_bb || !boot_vram) return;
    memcpy(boot_vram, boot_bb, boot_size);
}

static void boot_flush_rect(uint32 x, uint32 y, uint32 w, uint32 h) {
    if (!boot_bb || !boot_vram) return;
    if (x >= boot_w || y >= boot_h || w == 0 || h == 0) return;
    if (x + w > boot_w) w = boot_w - x;
    if (y + h > boot_h) h = boot_h - y;

    size row_bytes = w * 4;
    for (uint32 py = 0; py < h; py++) {
        uint8 *src = (uint8 *)boot_bb + (y + py) * boot_pitch + x * 4;
        uint8 *dst = (uint8 *)boot_vram + (y + py) * boot_pitch + x * 4;
        memcpy(dst, src, row_bytes);
    }
}

//fall back to boot GOP values before a backend is committed
static inline uint32 cur_width(void) { 
    return active_backend ? active_backend->width : boot_w; 
}

static inline uint32 cur_height(void) { 
    return active_backend ? active_backend->height : boot_h;
} 

static inline uint32 cur_pitch(void) { 
    return active_backend ? active_backend->pitch : boot_pitch; 
}

static void fb_fill_words_target(uint32 *dest, uint32 color, size count) {
    //if color is uniform (all bytes same) we can use memset
    uint8 b0 = color & 0xFF;
    uint8 b1 = (color >> 8) & 0xFF;
    uint8 b2 = (color >> 16) & 0xFF;
    uint8 b3 = (color >> 24) & 0xFF;

    if (b0 == b1 && b1 == b2 && b2 == b3) {
        memset(dest, b0, count * 4);
        return;
    }

    while (count--) {
        *dest++ = color;
    }
}

static void fb_copy_rect_target(uint32 *target, uint32 dst_x, uint32 dst_y, 
    uint32 src_x, uint32 src_y, uint32 w, uint32 h);

static void fb_fillrect_target(uint32 *target, uint32 x, uint32 y,
    uint32 w, uint32 h, uint32 color) {
    if (!target) return;
    //clamp to screen bounds
    if (x >= cur_width() || y >= cur_height()) return;
    if (x + w > cur_width()) w = cur_width() - x;
    if (y + h > cur_height()) h = cur_height() - y;

    for (uint32 py = y; py < y + h; py++) {
        uint32 *row = (uint32 *)((uint8 *)target + py * cur_pitch());
        fb_fill_words_target(&row[x], color, w);
    }
}

static void fb_scroll_target(uint32 *target, uint32 lines, uint32 bg_color) {
    if (!target || lines == 0 || lines >= cur_height()) return;
    fb_copy_rect_target(target, 0, 0, 0, lines, cur_width(), cur_height() - lines);
    fb_fillrect_target(target, 0, cur_height() - lines, cur_width(), lines, bg_color);
}

//$devices/fb0 object ops
static object_t *fb_object = NULL;

static ssize fb_obj_read(object_t *obj, void *buf, size len, size offset) {
    (void)obj;
    if (!active_backend) return 0;
    //bounds check
    if (offset >= active_backend->size) return 0;
    if (offset + len > active_backend->size) len = active_backend->size - offset;
    //read from frontbuffer (VRAM)
    memcpy(buf, (uint8 *)active_backend->display_buffer + offset, len);
    return len;
}

static ssize fb_obj_write(object_t *obj, const void *buf, size len, size offset) {
    (void)obj;
    if (!active_backend) return 0;
    //bounds check
    if (offset >= active_backend->size) return 0;
    if (len > active_backend->size - offset) len = active_backend->size - offset;
    size end = offset + len;
    //write to backbuffer/frontbuffer
    memcpy((uint8 *)active_backend->draw_buffer + offset, buf, len);
    //auto-flip after a complete frame write, including row-at-a-time writers
    if (end >= active_backend->size) fb_flip();
    return len;
}

static int fb_obj_stat(object_t *obj, stat_t *st) {
    (void)obj;
    if (!st || !active_backend) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DEVICE;
    st->size = active_backend->size;
    st->width = active_backend->width;
    st->height = active_backend->height;
    st->pitch = active_backend->pitch;
    st->ctime = st->mtime = st->atime = 0;
    return 0;
}

static object_ops_t fb_object_ops = {
    .read = fb_obj_read,
    .write = fb_obj_write,
    .stat = fb_obj_stat
};

//backend registration
bool fb_set_backend(fb_backend_t *backend) {
    if (active_backend) return false;
    active_backend = backend;
    return true;
}

bool fb_backend_active(void) {
    return active_backend != NULL;
}

//BUS-level init: capture the boot GOP address but don't commit yet so a GPU
//driver probed in the same BUS wave can claim the display first
void fb_init(void) {
    if (boot_vram) return;

    struct db_tag_framebuffer *fb = db_get_framebuffer();
    if (!fb) {
        puts("[fb] no framebuffer available\n");
        return;
    }

    boot_vram = (uint32 *)P2V(fb->address);
    boot_w = fb->width;
    boot_h = fb->height;
    boot_pitch = fb->pitch;
    boot_size = boot_h * boot_pitch;

    //remap VRAM as write-combining (huge performance boost on real hardware)
    mmu_map_range(
        mmu_get_kernel_pagemap(),
        (uintptr)boot_vram,
        fb->address,
        (boot_size + 0xFFF) / 0x1000,
        MMU_FLAG_WRITE | MMU_FLAG_WC
    );

    //fill the boot backend struct; backbuffer is allocated later in fb_init_backbuffer()
    boot_backend.name = "boot-gop";
    boot_backend.draw_buffer = boot_vram;
    boot_backend.display_buffer = boot_vram;
    boot_backend.width = boot_w;
    boot_backend.height = boot_h;
    boot_backend.pitch = boot_pitch;
    boot_backend.size = boot_size;
    boot_backend.flush = boot_flush;
    boot_backend.flush_rect = boot_flush_rect;
    boot_backend.cleanup = NULL;

    printf("[fb] boot GOP at 0x%llX %dx%d (WC mapped)\n", (uint64)fb->address, boot_w, boot_h);
}

//DEVICE-level init: commit the boot backend if no driver grabbed it,
//allocate its backbuffer, and register $devices/fb0
void fb_init_backbuffer(void) {
    if (!boot_vram) return;

    if (!active_backend) {
        active_backend = &boot_backend;
        printf("[fb] using boot-GOP backend (no GPU driver registered)\n");
    }

    if (active_backend == &boot_backend && !boot_bb) {
        boot_bb = kmalloc(boot_size);
        if (!boot_bb) {
            printf("[fb] WARN: failed to allocate %zu byte backbuffer, using direct VRAM\n", boot_size);
        } else {
            //preserve whatever was already on screen, this is only for if you
            //do any early-boot logging
            memcpy(boot_bb, boot_vram, boot_size);
            boot_backend.draw_buffer = boot_bb;
            printf("[fb] backbuffer allocated: %zu bytes\n", boot_size);
        }
    }

    if (!fb_object) {
        fb_object = object_create(OBJECT_DEVICE, &fb_object_ops, NULL);
        if (fb_object) ns_register("$devices/fb0", fb_object, HANDLE_RIGHTS_ALL);
    }

    printf("[fb] active backend: %s (%dx%d)\n",
        active_backend->name, active_backend->width, active_backend->height);
}

DECLARE_DRIVER(fb_init, INIT_LEVEL_BUS);
DECLARE_DRIVER(fb_init_backbuffer, INIT_LEVEL_DEVICE);

//return the draw target; before a backend commits we write straight to VRAM
static inline uint32 *get_draw_target(void) {
    if (active_backend) return (uint32 *)active_backend->draw_buffer;
    return boot_vram;
}

void fb_flip(void) {
    if (!active_backend || !active_backend->flush) return;
    active_backend->flush();
}

void fb_flip_rect(uint32 x, uint32 y, uint32 w, uint32 h) {
    if (!active_backend || !active_backend->flush_rect) return;
    if (x >= cur_width() || y >= cur_height() || w == 0 || h == 0) return;
    if (x + w > cur_width()) w = cur_width() - x;
    if (y + h > cur_height()) h = cur_height() - y;
    active_backend->flush_rect(x, y, w, h);
}

static void fb_copy_rect_target(uint32 *target, uint32 dst_x, uint32 dst_y,
    uint32 src_x, uint32 src_y, uint32 w, uint32 h) {
    if (!target || w == 0 || h == 0) return;

    uint32 fw = cur_width();
    uint32 fh = cur_height();
    uint32 fp = cur_pitch();

    if (dst_x >= fw || dst_y >= fh || src_x >= fw || src_y >= fh) return;
    if (src_x + w > fw) w = fw - src_x;
    if (dst_x + w > fw) w = fw - dst_x;
    if (src_y + h > fh) h = fh - src_y;
    if (dst_y + h > fh) h = fh - dst_y;

    size row_bytes = w * 4;
    bool reverse = (dst_y > src_y);

    if (reverse) {
        for (int32 py = (int32)h - 1; py >= 0; py--) {
            uint8 *src = (uint8 *)target + (src_y + (uint32)py) * fp + src_x * 4;
            uint8 *dst = (uint8 *)target + (dst_y + (uint32)py) * fp + dst_x * 4;
            memmove(dst, src, row_bytes);
        }
    } else {
        for (uint32 py = 0; py < h; py++) {
            uint8 *src = (uint8 *)target + (src_y + py) * fp + src_x * 4;
            uint8 *dst = (uint8 *)target + (dst_y + py) * fp + dst_x * 4;
            memmove(dst, src, row_bytes);
        }
    }
}

void fb_copy_rect(uint32 dst_x, uint32 dst_y, uint32 src_x, uint32 src_y, uint32 w, uint32 h) {
    fb_copy_rect_target(get_draw_target(), dst_x, dst_y, src_x, src_y, w, h);
}

bool fb_available(void) {
    return boot_vram != NULL || active_backend != NULL;
}

uint32 fb_width(void) { 
    return cur_width(); 
}

uint32 fb_height(void) { 
    return cur_height(); 
}

void fb_clear(uint32 color) {
    uint32 *target = get_draw_target();
    if (!target) return;

    if (cur_pitch() == cur_width() * 4) {
        fb_fill_words_target(target, color, cur_height() * cur_width());
    } else {
        for (uint32 y = 0; y < cur_height(); y++) {
            uint32 *row = (uint32 *)((uint8 *)target + y * cur_pitch());
            fb_fill_words_target(row, color, cur_width());
        }
    }
}

void fb_putpixel(uint32 x, uint32 y, uint32 color) {
    uint32 *target = get_draw_target();
    if (!target) return;
    if (x >= cur_width() || y >= cur_height()) return;
    uint32 *row = (uint32 *)((uint8 *)target + y * cur_pitch());
    row[x] = color;
}

void fb_fillrect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color) {
    fb_fillrect_target(get_draw_target(), x, y, w, h, color);
}

void fb_drawimage(const unsigned char *src, uint32 width, uint32 height,
    uint32 offset_x, uint32 offset_y) {
    uint32 *target = get_draw_target();
    if (!target || !src) return;
    if (offset_x >= cur_width() || offset_y >= cur_height()) return;
    if (offset_x + width > cur_width()) width = cur_width() - offset_x;
    if (offset_y + height > cur_height()) height = cur_height() - offset_y;

    for (uint32 y = 0; y < height; y++) {
        uint32 *row = (uint32 *)((uint8 *)target + (offset_y + y) * cur_pitch()) + offset_x;
        for (uint32 x = 0; x < width; x++) {
            uint8 r = *src++;
            uint8 g = *src++;
            uint8 b = *src++;
            row[x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

//draw a glyph from a font
//width must be <= 8 as each row is represented by a single uint8
void fb_drawglyph(uint32 x, uint32 y, const uint8 *glyph, uint32 fg, uint32 bg,
    uint32 width, uint32 height) {
    uint32 *target = get_draw_target();
    if (!target || !glyph) return;
    if (x >= cur_width() || y >= cur_height()) return;
    if (width > 8) width = 8;
    if (x + width > cur_width()) width = cur_width() - x;
    if (y + height > cur_height()) height = cur_height() - y;

    for (uint32 py = 0; py < height; py++) {
        uint8 row_bits = glyph[py];
        uint32 *row = (uint32 *)((uint8 *)target + (y + py) * cur_pitch()) + x;
        for (uint32 px = 0; px < width; px++) {
            row[px] = (row_bits & (0x80 >> px)) ? fg : bg;
        }
    }
}

void fb_scroll(uint32 lines, uint32 bg_color) {
    if (lines == 0 || lines >= cur_height()) return;

    uint32 *draw = get_draw_target();
    uint32 *disp = active_backend ? active_backend->display_buffer : boot_vram;

    if (draw != disp && disp) {
        //double-buffered: scroll both so they stay in sync
        fb_scroll_target(draw, lines, bg_color);
        fb_scroll_target(disp, lines, bg_color);
        return;
    }

    fb_scroll_target(draw, lines, bg_color);
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
        if (e2 < dx)  { 
            err += dx; 
            y1 += sy; 
        }
    }
}
