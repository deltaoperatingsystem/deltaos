#include <system.h>
#include <string.h>
#include <io.h>
#include <pixie.h>

#define NUM_WINS 4

typedef struct {
    px_window_t *win;
    px_surface_t *surf;
    uint16 w, h;
    uint32 bg, accent;
    int mx, my;
    bool pressed;
    bool key_flash;
    uint64 frames;
    int phase;
} test_win_t;

static test_win_t wins[NUM_WINS];

static const uint32 colors[NUM_WINS][2] = {
    { PX_RGB(60, 20, 20), PX_RGB(200, 60, 60) },
    { PX_RGB(20, 60, 20), PX_RGB(60, 200, 60) },
    { PX_RGB(20, 20, 60), PX_RGB(60, 60, 200) },
    { PX_RGB(60, 60, 20), PX_RGB(200, 200, 60) },
};

//sync cached geometry with the live surface mapping before drawing
static void sync_window_size(test_win_t *tw) {
    if (!tw || !tw->surf) return;

    tw->w = px_get_surface_w(tw->surf);
    tw->h = px_get_surface_h(tw->surf);

    if (tw->w == 0 || tw->h == 0) {
        tw->mx = 0;
        tw->my = 0;
        return;
    }

    if (tw->mx >= (int)tw->w) tw->mx = tw->w - 1;
    if (tw->my >= (int)tw->h) tw->my = tw->h - 1;
    if (tw->mx < 0) tw->mx = 0;
    if (tw->my < 0) tw->my = 0;
}

static void ensure_gui(void) {
    //check if compositor is already running
    for (int i = 0; i < 20; i++) {
        if (px_init()) goto wm_start;
        yield();
    }

    //spawn compositor
    puts("[test_wm] spawning compositor...\n");
    if (spawn("/system/binaries/compositor", 0, NULL) < 0) {
        puts("[test_wm] failed to spawn compositor\n");
        exit(1);
    }
    for (int i = 0; i < 100; i++) {
        if (px_init()) goto wm_start;
        yield();
    }
    puts("[test_wm] compositor did not start\n");
    exit(1);

wm_start:
    //spawn WM and give it time to claim
    puts("[test_wm] spawning WM...\n");
    if (spawn("/system/binaries/wm", 0, NULL) < 0) {
        puts("[test_wm] failed to spawn WM\n");
        exit(1);
    }
    for (int i = 0; i < 100; i++) yield();
}

int main(void) {
    ensure_gui();
    puts("[test_wm] compositor+WM ready\n");

    for (int i = 0; i < NUM_WINS; i++) {
        test_win_t *tw = &wins[i];
        tw->win = px_create_window("test", 400, 300);
        if (!tw->win) exit(1);
        tw->surf = px_get_surface(tw->win);
        if (!tw->surf) exit(1);

        char title[32];
        snprintf(title, sizeof(title), "Window %d", i + 1);
        px_set_title(tw->win, title);

        tw->w = px_get_surface_w(tw->surf);
        tw->h = px_get_surface_h(tw->surf);
        tw->bg = colors[i][0];
        tw->accent = colors[i][1];
        tw->mx = tw->w / 2;
        tw->my = tw->h / 2;
    }

    while (1) {
        for (int i = 0; i < NUM_WINS; i++) {
            test_win_t *tw = &wins[i];
            px_event_t ev;
            while (px_poll_event(tw->win, &ev)) {
                if (ev.type == PX_EVENT_MOUSE) {
                    tw->mx = ev.mouse.x;
                    tw->my = ev.mouse.y;
                    tw->pressed = ev.mouse.buttons & PX_MOUSE_BTN_LEFT;
                } else if (ev.type == PX_EVENT_KEY) {
                    kbd_event_t *ke = &ev.key.data;
                    if (ke->pressed) tw->key_flash = true;
                } else if (ev.type == PX_EVENT_RESIZE) {
                    tw->w = ev.resize.w;
                    tw->h = ev.resize.h;
                    sync_window_size(tw);
                }
            }

            sync_window_size(tw);

            if (tw->w == 0 || tw->h == 0) continue;

            //fill background
            px_draw_rect(tw->surf, px_create_rect(0, 0, tw->w, tw->h, tw->bg));

            //inner border - accent color with a 1px black gap
            px_draw_rect(tw->surf, px_create_rect(3, 3, tw->w - 6, tw->h - 6, PX_RGB(0, 0, 0)));
            px_draw_rect(tw->surf, px_create_rect(4, 4, tw->w - 8, tw->h - 8, tw->accent));

            //crosshair cursor
            uint32 cursor_col = tw->pressed ? PX_RGB(255, 255, 255) : tw->accent;
            px_draw_rect(tw->surf, px_create_rect(tw->mx - 15, tw->my - 1, 30, 2, cursor_col));
            px_draw_rect(tw->surf, px_create_rect(tw->mx - 1, tw->my - 15, 2, 30, cursor_col));

            //keyboard flash ring around crosshair center
            if (tw->key_flash) {
                px_draw_rect(tw->surf, px_create_rect(tw->mx - 8, tw->my - 8, 16, 16, PX_RGB(255, 255, 100)));
                tw->key_flash = false;
            }

            //frame counter bar at bottom
            tw->frames++;
            uint16 bar_w = (tw->frames % 200) < 100 ? 40 : 20;
            px_draw_rect(tw->surf, px_create_rect(tw->w / 2 - bar_w / 2, tw->h - 12, bar_w, 6, tw->accent));

            px_update_window(tw->win);
        }
        yield();
    }

    return 0;
}
