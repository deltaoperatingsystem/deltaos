#ifndef PIXIE_H
#define PIXIE_H

typedef struct px_surface px_surface_t;
typedef struct px_window px_window_t;
typedef struct px_rect px_rect_t;

void px_init();

px_window_t *px_create_window(char *name, uint16 width, uint16 height);
px_surface_t *px_get_surface(px_window_t *win);
int16 px_get_surface_w(px_surface_t *surface);
int16 px_get_surface_h(px_surface_t *surface);

px_rect_t *px_create_rect(uint16 x, uint16 y, uint16 width, uint16 height, uint32 colour);

void px_draw_pixel(px_surface_t *surface, uint32 x, uint32 y, uint32 colour);
void px_draw_rect(px_surface_t *surface, px_rect_t *rect);
void px_update_window(px_window_t *win);

#endif