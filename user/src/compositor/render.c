#include "render.h"
#include "surface.h"

//damage tracking helpers
void damage_add_rect(int16 x, int16 y, int16 w, int16 h) {
    if (w <= 0 || h <= 0) return;
    int16 x1 = x + w;
    int16 y1 = y + h;
    if (comp.has_damage) {
        if (x < comp.damage_x0) comp.damage_x0 = x;
        if (y < comp.damage_y0) comp.damage_y0 = y;
        if (x1 > comp.damage_x1) comp.damage_x1 = x1;
        if (y1 > comp.damage_y1) comp.damage_y1 = y1;
    } else {
        comp.damage_x0 = x;
        comp.damage_y0 = y;
        comp.damage_x1 = x1;
        comp.damage_y1 = y1;
        comp.has_damage = true;
    }
}

void damage_add_surface_rect(surface_t *s) {
    if (!s || !s->alive) return;
    int16 dx = s->x - s->deco.border_w;
    int16 dy = s->y - s->deco.titlebar_h - s->deco.border_w;
    int16 dw = s->content_w + 2 * s->deco.border_w;
    int16 dh = s->content_h + s->deco.titlebar_h + s->deco.border_w;
    damage_add_rect(dx, dy, dw, dh);
}

//clipped filled rectangle in framebuffer coordinates
void fill_rect(uint32 *fb, int x, int y, int w, int h, uint32 color) {
    int rx0 = max(x, (int)comp.damage_x0);
    int ry0 = max(y, (int)comp.damage_y0);
    int rx1 = min(x + w, (int)comp.damage_x1);
    int ry1 = min(y + h, (int)comp.damage_y1);
    for (int row = ry0; row < ry1; row++) {
        for (int col = rx0; col < rx1; col++) {
            fb[row * comp.row_stride + col] = color;
        }
    }
}

//8x16 bitmap font MSB of each byte is the leftmost pixel
static void draw_glyph(uint32 *fb, int x, int y, unsigned char ch, uint32 color) {
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8 *glyph = &font[(uint8)ch * 16];
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < comp.damage_y0 || py >= comp.damage_y1) continue;
        for (int col = 0; col < 8; col++) {
            if (!(glyph[row] & (0x80u >> col))) continue;
            int px = x + col;
            if (px < comp.damage_x0 || px >= comp.damage_x1) continue;
            fb[py * comp.row_stride + px] = color;
        }
    }
}

//nearest neighbour scale for wallpaper fitting
static void scale_nn(const uint32 *src, int sw, int sh, uint32 *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            if (sx >= sw) sx = sw - 1;
            dst[y * dw + x] = src[sy * sw + sx];
        }
    }
}

void load_wallpaper(void) {
    handle_t h = get_obj(INVALID_HANDLE, "$files/wallpaper.dm", RIGHT_READ | RIGHT_GET_INFO);
    if (h == INVALID_HANDLE) { comp.wallpaper_loaded = false; return; }

    stat_t st;
    if (fstat(h, &st) != 0) { handle_close(h); comp.wallpaper_loaded = false; return; }

    uint8 *filebuf = malloc(st.size);
    if (!filebuf || handle_read(h, filebuf, st.size) != (ssize)st.size) {
        free(filebuf); handle_close(h); comp.wallpaper_loaded = false; return;
    }
    handle_close(h);

    dm_image_t img;
    if (dm_load_image(filebuf, st.size, &img) != 0) {
        free(filebuf); comp.wallpaper_loaded = false; return;
    }

    if (img.width == 0 || img.height == 0 || img.pixel_format != DM_PIXEL_RGBA32) {
        free(img.pixels); free(filebuf); comp.wallpaper_loaded = false; return;
    }

    size img_pixels = (size)img.width * (size)img.height * sizeof(uint32);
    uint32 *tmp = malloc(img_pixels);
    if (!tmp) { free(img.pixels); free(filebuf); comp.wallpaper_loaded = false; return; }
    memcpy(tmp, img.pixels, img_pixels);
    free(img.pixels);
    free(filebuf);

    if (img.width == comp.screen_w && img.height == comp.screen_h) {
        comp.wallpaper = tmp;
        comp.wallpaper_w = comp.screen_w;
        comp.wallpaper_h = comp.screen_h;
        comp.wallpaper_loaded = true;
    } else {
        uint32 *scaled = malloc((size)comp.screen_w * comp.screen_h * sizeof(uint32));
        if (!scaled) { free(tmp); comp.wallpaper_loaded = false; return; }
        scale_nn(tmp, img.width, img.height, scaled, comp.screen_w, comp.screen_h);
        free(tmp);
        comp.wallpaper = scaled;
        comp.wallpaper_w = comp.screen_w;
        comp.wallpaper_h = comp.screen_h;
        comp.wallpaper_loaded = true;
    }

    //DM stores RGBA byte order and FB expects BGR
    for (uint32 i = 0; i < (uint32)comp.wallpaper_h * comp.wallpaper_w; i++) {
        uint32 p = comp.wallpaper[i];
        uint8 r = (uint8)(p);
        uint8 g = (uint8)(p >> 8);
        uint8 b = (uint8)(p >> 16);
        comp.wallpaper[i] = FB_RGB(r, g, b);
    }
    INFO("Wallpaper loaded\n");
}

static void render_wallpaper(uint32 *fb) {
    int x0 = comp.damage_x0;
    int y0 = comp.damage_y0;
    int x1 = comp.damage_x1;
    int y1 = comp.damage_y1;
    if (!comp.wallpaper_loaded || !comp.wallpaper) {
        for (int y = y0; y < y1; y++) {
            memset(fb + y * comp.row_stride + x0, 0, (x1 - x0) * sizeof(uint32));
        }
        return;
    }
    for (int y = y0; y < y1; y++) {
        memcpy(fb + y * comp.row_stride + x0, comp.wallpaper + y * comp.screen_w + x0, (x1 - x0) * sizeof(uint32));
    }
}

static void draw_decoration(uint32 *fb, surface_t *s) {
    if (!s->alive) return;

    bool foc = s->focused;
    int dx = (int)s->x - (int)s->deco.border_w;
    int dy = (int)s->y - (int)s->deco.titlebar_h - (int)s->deco.border_w;
    int dw = (int)s->content_w + 2 * (int)s->deco.border_w;
    int dh = (int)s->content_h + (int)s->deco.titlebar_h + (int)s->deco.border_w;

    //check if decoration bounds overlap damage bounding box
    if (dx >= comp.damage_x1 || dy >= comp.damage_y1 || dx + dw <= comp.damage_x0 || dy + dh <= comp.damage_y0)
        return;

    uint32 tb = foc ? s->deco.tb_focused : s->deco.tb_unfocused;
    uint32 bd = foc ? s->deco.bd_focused : s->deco.bd_unfocused;
    uint32 tx = foc ? s->deco.tx_focused : s->deco.tx_unfocused;

    //title bar background
    fill_rect(fb, dx, dy, dw, s->deco.titlebar_h, tb);
    //top accent line
    fill_rect(fb, dx, dy, dw, 1, bd);
    //left right and bottom borders
    fill_rect(fb, dx, dy, s->deco.border_w, dh, bd);
    fill_rect(fb, dx + dw - s->deco.border_w, dy, s->deco.border_w, dh, bd);
    fill_rect(fb, dx, dy + dh - s->deco.border_w, dw, s->deco.border_w, bd);

    //close button in the top right corner
    if (s->deco.show_close) {
        int btn = 10;
        int bx = dx + dw - s->deco.border_w - btn - 4;
        int by = dy + (s->deco.titlebar_h - btn) / 2;
        fill_rect(fb, bx, by, btn, btn, s->deco.close_btn);
    }

    //title text truncated to fit between the close button and left edge
    const char *title = s->title[0] ? s->title : "Window";
    int tx_x = dx + s->deco.border_w + 6;
    int tx_y = dy + (s->deco.titlebar_h - 16) / 2 + 1;
    int btn_x = dx + dw - s->deco.border_w - 10 - 4;
    int max_chars = (btn_x - tx_x - 4) / 8;
    for (int k = 0; k < max_chars && title[k]; k++)
        draw_glyph(fb, tx_x + k * 8, tx_y, (unsigned char)title[k], tx);
}

void render_surfaces(uint32 *fb) {
    render_wallpaper(fb);

    for (int si = 0; si < comp.stack_count; si++) {
        int idx = find_surface(comp.stack[si]);
        if (idx < 0) continue;
        surface_t *s = &comp.surfaces[idx];
        if (!s->alive) continue;

        if (s->pixels) {
            int dst_x0 = max(max((int)s->x, 0), (int)comp.damage_x0);
            int dst_y0 = max(max((int)s->y, 0), (int)comp.damage_y0);
            int dst_x1 = min(min((int)(s->x + s->content_w), comp.screen_w), (int)comp.damage_x1);
            int dst_y1 = min(min((int)(s->y + s->content_h), comp.screen_h), (int)comp.damage_y1);
            if (dst_x0 < dst_x1 && dst_y0 < dst_y1) {
                int copy_w = dst_x1 - dst_x0;
                int copy_h = dst_y1 - dst_y0;
                int src_x0 = dst_x0 - (int)s->x;
                int src_y0 = dst_y0 - (int)s->y;
                if (src_x0 < 0) src_x0 = 0;
                if (src_y0 < 0) src_y0 = 0;

                if (copy_w > (int)s->w - src_x0) copy_w = (int)s->w - src_x0;
                if (copy_h > (int)s->h - src_y0) copy_h = (int)s->h - src_y0;

                if (copy_w > 0 && copy_h > 0) {
                    for (int row = 0; row < copy_h; row++) {
                        uint32 *src_row = s->pixels + (size)(src_y0 + row) * s->w + src_x0;
                        uint32 *dst_row = fb + (size)(dst_y0 + row) * comp.row_stride + dst_x0;
                        for (int col = 0; col < copy_w; col++) {
                            dst_row[col] = src_row[col];
                        }
                    }
                }
            }
        }
        draw_decoration(fb, s);
    }
}

void render_mouse(uint32 *fb) {
    //software cursor 0 means transparent pixel from cursor data
    for (int i = 0; i < cursor_get_width(); i++) {
        for (int j = 0; j < cursor_get_height(); j++) {
            uint32 c = cursor_get_pixel(i, j);
            if (c == 0) continue;
            int px = i + (int)comp.mouse_x;
            int py = j + (int)comp.mouse_y;
            if (px < (int)comp.damage_x0 || py < (int)comp.damage_y0 || px >= (int)comp.damage_x1 || py >= (int)comp.damage_y1) continue;
            fb[py * comp.row_stride + px] = c;
        }
    }
}
