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

bool px_init();

px_window_t *px_create_window(char *name, uint16 width, uint16 height);
px_surface_t *px_get_surface(px_window_t *win);
uint16 px_get_surface_w(px_surface_t *surface);
uint16 px_get_surface_h(px_surface_t *surface);

//constructors
static inline px_rect_t px_make_rect(uint16 x, uint16 y, uint16 w, uint16 h, uint32 c) {
    return (px_rect_t){.x=x,.y=y,.w=w,.h=h,.c=c};
}

void px_draw_rect(px_surface_t *surface, px_rect_t rect);
void px_update_window(px_window_t *win);

#endif