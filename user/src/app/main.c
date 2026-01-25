#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

int main(void) {
    px_init();
    px_window_t *w = px_create_window("Test Window", 500, 500);
    px_surface_t *surface = px_get_surface(w);

    //test
    while (1) {
        px_rect_t *rect = px_create_rect(0, 0, px_get_surface_w(surface), px_get_surface_h(surface), 0xFFFFFFFF);
        px_draw_rect(surface, rect);
        px_update_window(w);

        yield();
    }

    return 0;
}