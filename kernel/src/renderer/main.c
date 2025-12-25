#include <drivers/fb.h>
#include <lib/time.h>
#include <drivers/console.h>

#define SIZE    10
#define FPS     60

static double dz = 0;

typedef struct {
    double x;
    double y;
    double z;
} point;

point screen(point p) {
    point n;
    n.x = ((p.x + 1) / 2 ) * fb_width();
    n.y = (1 - ((p.y + 1) / 2 )) * fb_height();
    return n;
}

void draw_point(point p) {
    fb_fillrect(p.x - SIZE / 2, p.y - SIZE / 2, SIZE, SIZE, FB_WHITE);
}

point project(point p) {
    return (point){
        .x = p.x / p.z,
        .y = p.y / p.z,
    };
}

const point vs[] = {
    { .x = 0.5,     .y = 0.5,   .z = 1},
    { .x = -0.5,    .y = 0.5,   .z = 1},
    { .x = 0.5,     .y = -0.5,  .z = 1},
    { .x = -0.5,    .y = -0.5,  .z = 1},
};

point translate_z(point p, double dz) {
    return (point){
        .x = p.x,
        .y = p.y,
        .z = p.z + dz,
    };
}

void frame() {
    double dt = 1.0/FPS;
    dz += dt;
    fb_clear(FB_BLACK);
    for (int i = 0; i < 4; i++) {
        draw_point(screen(project(translate_z(vs[i], dz))));
    }
}

void rmain(void) {
    while (1) {
        frame();
        con_flush();
        sleep(1000 / FPS);
    }
}