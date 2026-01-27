#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

int main(void) {
    if (!px_init()) exit(1);
    px_window_t *w = px_create_window("Test Window", 500, 500); if (!w) exit(1);
    px_surface_t *surface = px_get_surface(w); if (!surface) exit(1);

    while (1) {
        px_rect_t rect = px_create_rect(0, 0, 500, 500, 0xFFFFFFFF);
        px_draw_rect(surface, rect);
        px_update_window(w);

        yield();
    }

    return 0;
}