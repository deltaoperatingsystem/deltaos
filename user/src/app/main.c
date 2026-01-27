#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

/**
 * Program entry point that initializes the Pixie subsystem, creates a 500×500 window
 * with its drawing surface, and enters an infinite render loop that fills the window
 * with a white rectangle each frame.
 *
 * The function calls exit(1) if initialization, window creation, or surface acquisition fails.
 *
 * @returns 0 (unreachable under normal execution because the function enters an infinite loop).
 */
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