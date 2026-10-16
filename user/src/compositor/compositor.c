#include "compositor.h"
#include "server.h"
#include "render.h"
#include "input.h"

//global compositor state, declared extern in compositor.h
struct compositor_state comp;

#define COMP_INIT_CAP 16

static bool grow_ptr(void **ptr, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return true;
    size_t ncap = *cap ? *cap : COMP_INIT_CAP;
    while (ncap < need) {
        if (ncap > (size_t)-1 / 2 / (elem ? elem : 1)) return false;
        ncap *= 2;
    }
    void *np = realloc(*ptr, ncap * elem);
    if (!np) return false;
    *ptr = np;
    *cap = ncap;
    return true;
}

bool comp_ensure_surfaces(size_t need) {
    return grow_ptr((void **)&comp.surfaces, &comp.surfaces_cap, need, sizeof(surface_t));
}

bool comp_ensure_stack(size_t need) {
    return grow_ptr((void **)&comp.stack, &comp.stack_cap, need, sizeof(surface_id_t));
}

bool comp_ensure_clients(size_t need) {
    if (!grow_ptr((void **)&comp.clients, &comp.clients_cap, need, sizeof(handle_t))) return false;
    if (!grow_ptr((void **)&comp.client_pids, &comp.client_pids_cap, need, sizeof(uint32))) return false;
    return true;
}

void send_msg(handle_t ch, comp_msg_t *msg) {
    if (ch == INVALID_HANDLE) return;
    channel_send(ch, msg, sizeof(comp_msg_t));
}

int recv_msg(handle_t ch, comp_msg_t *msg, uint32 *sender_pid) {
    channel_recv_result_t res;
    int rc = channel_try_recv_msg(ch, msg, sizeof(comp_msg_t), NULL, 0, &res);
    if (rc == 0) {
        if (res.data_len != sizeof(comp_msg_t)) {
            return -1;
        }
        if (sender_pid) *sender_pid = res.sender_pid;
    }
    return rc;
}

static void fb_setup(void) {
    comp.fb_handle = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_READ | RIGHT_WRITE);
    ASSERT(comp.fb_handle == INVALID_HANDLE, "Failed to get framebuffer\n");

    stat_t st;
    if (stat("$devices/fb0", &st) >= 0 && st.width > 0 && st.height > 0 &&
        st.pitch >= st.width * sizeof(uint32) && (st.pitch % sizeof(uint32)) == 0) {
        comp.fb_size = st.height * st.pitch;
        comp.screen_w = st.width;
        comp.screen_h = st.height;
        comp.screen_bpp = sizeof(uint32);
        comp.screen_pitch_bytes = st.pitch;
        comp.row_stride = st.pitch / sizeof(uint32);
    } else {
        comp.screen_w = 1280;
        comp.screen_h = 800;
        comp.fb_size = comp.screen_w * comp.screen_h * sizeof(uint32);
        comp.screen_bpp = sizeof(uint32);
        comp.screen_pitch_bytes = comp.screen_w * sizeof(uint32);
        comp.row_stride = comp.screen_w;
    }

    comp.backbuffer = malloc(comp.fb_size);
    ASSERT(!comp.backbuffer, "Failed to allocate backbuffer\n");

    comp.saved_fb = malloc(comp.fb_size);
    ASSERT(!comp.saved_fb, "Failed to allocate saved_fb\n");
    handle_seek(comp.fb_handle, 0, HANDLE_SEEK_SET);
    handle_read(comp.fb_handle, comp.saved_fb, comp.fb_size);

    INFO("Framebuffer %dx%d@%d\n", comp.screen_w, comp.screen_h, comp.screen_bpp);
}

static void server_setup(void) {
    handle_t client_end;
    ASSERT(channel_create(&comp.server_handle, &client_end) != 0 ||
           comp.server_handle == INVALID_HANDLE || client_end == INVALID_HANDLE,
           "Failed to create server channel\n");
    //ceiling: any process can connect to the server but can only send/recv on it
    ASSERT(ns_register("$gui/display/server", client_end, RIGHT_READ | RIGHT_WRITE) != 0,
           "Failed to register server channel\n");
    handle_close(client_end);
    INFO("Server channel at $gui/display/server\n");
}

static void set_vt_cursor_visible(bool visible) {
    if (comp.vt_handle == INVALID_HANDLE) return;
    char cmd[3] = {27, 'v', visible ? '1' : '0'};
    handle_write(comp.vt_handle, cmd, sizeof(cmd));
}

int main(void) {
    memset(&comp, 0, sizeof(comp));
    comp.wm_ch = INVALID_HANDLE;
    comp.mouse_h = INVALID_HANDLE;
    comp.vt_handle = INVALID_HANDLE;
    comp.next_id = 1;
    if (!comp_ensure_surfaces(COMP_INIT_CAP) ||
        !comp_ensure_stack(COMP_INIT_CAP) ||
        !comp_ensure_clients(COMP_INIT_CAP)) {
        dprintf("[compositor]: ERROR: out of memory at startup\n");
        exit(1);
    }
    for (size_t i = 0; i < comp.clients_cap; i++) comp.clients[i] = INVALID_HANDLE;

    fb_setup();
    comp.vt_handle = get_obj(INVALID_HANDLE, "$devices/vt0", RIGHT_WRITE);
    set_vt_cursor_visible(false); //hide VT cursor
    kbd_init();
    load_wallpaper();
    server_setup();

    //damage entire screen initially
    damage_add_rect(0, 0, comp.screen_w, comp.screen_h);

    while (1) {
        server_listen();
        handle_input();

        if (comp.has_damage) {
            comp.has_damage = false;

            //render full screen to avoid partial update artifacts with overlapping surfaces
            comp.damage_x0 = 0;
            comp.damage_y0 = 0;
            comp.damage_x1 = comp.screen_w;
            comp.damage_y1 = comp.screen_h;

            render_surfaces(comp.backbuffer);
            render_mouse(comp.backbuffer);
            handle_seek(comp.fb_handle, 0, HANDLE_SEEK_SET);
            handle_write(comp.fb_handle, comp.backbuffer, comp.fb_size);
        }
        yield();
    }

    return 0;
}
