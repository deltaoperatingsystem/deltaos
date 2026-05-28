#ifndef RENDER_H
#define RENDER_H

#include "compositor.h"

//framebuffer space solid fill with clipping
void fill_rect(uint32 *fb, int x, int y, int w, int h, uint32 color);
//loads and converts wallpaper into compositor FB format
void load_wallpaper(void);
//composites wallpaper then surfaces then decorations
void render_surfaces(uint32 *fb);
//software cursor draw pass
void render_mouse(uint32 *fb);

//damage tracking helpers
void damage_add_rect(int16 x, int16 y, int16 w, int16 h);
void damage_add_surface_rect(surface_t *s);

#endif
