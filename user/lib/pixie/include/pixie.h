#ifndef PIXIE_H
#define PIXIE_H

#include <types.h>

typedef struct px_surface px_surface_t;
typedef struct px_window px_window_t;
typedef struct px_rect {
    uint16 x, y;
    uint16 w, h;
    uint32 c;
} px_rect_t;
typedef struct px_image px_image_t;

#define PX_RGB(r, g, b) ((uint32)((b) | ((g) << 8) | ((r) << 16)))

bool px_init();

px_window_t *px_create_window(char *name, uint16 width, uint16 height);
px_surface_t *px_get_surface(px_window_t *win);
uint16 px_get_surface_w(px_surface_t *surface);
uint16 px_get_surface_h(px_surface_t *surface);

//constructors
static inline px_rect_t px_create_rect(uint16 x, uint16 y, uint16 w, uint16 h, uint32 c) {
    return (px_rect_t){.x=x,.y=y,.w=w,.h=h,.c=c};
}

bool px_draw_pixel(px_surface_t *surface, uint32 x, uint32 y, uint32 colour);
void px_draw_rect(px_surface_t *surface, px_rect_t rect);

px_image_t *px_load_image(char *path);
bool px_draw_image(px_surface_t *surface, px_image_t *image, uint32 x, uint32 y);

void px_update_window(px_window_t *win);

#endif