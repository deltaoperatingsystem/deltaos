#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

int main(void) {
    if (!px_init()) exit(1);
    px_window_t *w = px_create_window("Test Window", 500, 500); if (!w) exit(1);
    px_surface_t *surface = px_get_surface(w); if (!surface) exit(1);

    px_set_title(w, "Hello DeltaOS");

    uint16 sw = px_get_surface_w(surface);
    uint16 sh = px_get_surface_h(surface);

    int mx = 0, my = 0;
    bool pressed = false;

    while (1) {
        px_event_t ev;
        while (px_poll_event(w, &ev)) {
            if (ev.type == PX_EVENT_MOUSE) {
                mx = ev.mouse.x;
                my = ev.mouse.y;
                pressed = ev.mouse.buttons & PX_MOUSE_BTN_LEFT;
            } else if (ev.type == PX_EVENT_RESIZE) {
                sw = ev.resize.w;
                sh = ev.resize.h;
                if (mx >= (int)sw) mx = sw ? (int)sw - 1 : 0;
                if (my >= (int)sh) my = sh ? (int)sh - 1 : 0;
            }
        }

        px_draw_rect(surface, px_create_rect(0, 0, sw, sh, PX_RGB(10, 10, 20)));

        uint16 inner_w = sw > 40 ? sw - 40 : 0;
        uint16 inner_h = sh > 40 ? sh - 40 : 0;
        px_draw_rect(surface, px_create_rect(20, 20, inner_w, inner_h, PX_RGB(30, 35, 60)));

        px_draw_rect(surface, px_create_rect(mx - 5, my - 5, 10, 10,
            pressed ? PX_RGB(255, 80, 80) : PX_RGB(80, 200, 255)));

        px_update_window(w);

        yield();
    }

    return 0;
}