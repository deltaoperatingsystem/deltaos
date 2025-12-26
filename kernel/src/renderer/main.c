#include <drivers/fb.h>
#include <lib/time.h>
#include <drivers/console.h>
#include <lib/math.h>
#include <drivers/keyboard.h>

#define SIZE    10
#define FPS     60

typedef struct {
    double x;
    double y;
    double z;
} point;

point screen(point p) {
    point n;
    if (p.x > 1) p.x = 1;
    if (p.x < -1) p.x = -1;
    if (p.y > 1) p.y = 1;
    if (p.y < -1) p.y = -1;
    n.x = ((p.x + 1) / 2 ) * fb_height() + (fb_width() - fb_height()) / 2;
    n.y = (1 - ((p.y + 1) / 2 )) * fb_height();
    return n;
}

void draw_point(point p) {
    fb_fillrect(p.x - SIZE / 2, p.y - SIZE / 2, SIZE, SIZE, FB_WHITE);
}

void line(point a, point b) {
    fb_drawline(a.x, a.y, b.x, b.y, FB_WHITE);
}

point project(point p) {
    return (point){
        .x = p.x / p.z,
        .y = p.y / p.z,
    };
}

const point vs[] = {
    { .x = 0.25,     .y = 0.25,   .z = 0.25},
    { .x = -0.25,    .y = 0.25,   .z = 0.25},
    { .x = -0.25,    .y = -0.25,  .z = 0.25},
    { .x = 0.25,     .y = -0.25,  .z = 0.25},

    { .x = 0.25,     .y = 0.25,   .z = -0.25},
    { .x = -0.25,    .y = 0.25,   .z = -0.25},
    { .x = -0.25,    .y = -0.25,  .z = -0.25},
    { .x = 0.25,     .y = -0.25,  .z = -0.25},
};

const int es[12][2] = {
    {0, 1},
    {1, 2},
    {2, 3},
    {3, 0},
    {4, 5},
    {5, 6},
    {6, 7},
    {7, 4},
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7}
};

point translate_z(point p, double dz) {
    return (point){
        .x = p.x,
        .y = p.y,
        .z = p.z + dz,
    };
}

point rotate_xz(point p, double theta) {
    return (point){
        .x = p.x * cos(theta) - p.z * sin(theta),
        .y = p.y,
        .z = p.x * sin(theta) + p.z * cos(theta)
    };
}

static double dz = 1;
static double angle = 0;

void frame() {
    double dt = 1.0/FPS;
    // dz += dt;
    // angle += M_PI * dt * 0.5;
    if (get_keystate('w')) dz += dt;
    if (get_keystate('s')) dz -= dt;
    if (get_keystate('a')) angle += M_PI * dt * 0.5;
    if (get_keystate('d')) angle -= M_PI * dt * 0.5;
    fb_clear(FB_BLACK);
    for (int i = 0; i < 12; i++) {
            const point a = vs[es[i][0]];
            const point b = vs[es[i][1]];

            line(screen(project(translate_z(rotate_xz(a, angle), dz))),
                screen(project(translate_z(rotate_xz(b, angle), dz))));
    }
}

void rmain(void) {
    while (1) {
        frame();
        con_flush();
        sleep(1000 / FPS);
    }
}