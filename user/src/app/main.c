#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

int main(void) {
    if (!px_init()) exit(1);
    px_window_t *w = px_create_window("Test Window", 500, 500); if (!w) exit(1);
    px_surface_t *surface = px_get_surface(w); if (!surface) exit(1);
    // px_image_t *image = px_load_image("$files/wallpaper.dm");

    px_rect_t rect = px_create_rect(0, 0, 10, 10, 0xFFFFFFFF);
    while (1) {
        // px_draw_image(surface, image, 0, 0);
        // px_draw_rect(surface, rect);
        // px_update_window(w);

        yield();
    }

    return 0;
}