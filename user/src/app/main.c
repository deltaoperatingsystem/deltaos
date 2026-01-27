#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

int main(void) {
    px_init();
    px_window_t *w = px_create_window("Test Window", 500, 500);
    px_surface_t *surface = px_get_surface(w);

    while (1) {
        px_rect_t rect = (px_rect_t){.x=0,.y=0,.w=500,.h=500,.c=0xFFFFFFFF};
        px_draw_rect(surface, &rect);
        px_update_window(w);

        yield();
    }

    return 0;
}