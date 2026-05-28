#include "wm.h"
#include "layout.h"
#include "keybind.h"
#include <keyboard.h>

//global WM state
handle_t wm_ch = INVALID_HANDLE;
wm_client_t clients[MAX_CLIENTS];
uint8 num_clients = 0;
surface_id_t focused = 0;

int find_client(surface_id_t id) {
    for (int i = 0; i < num_clients; i++)
        if (clients[i].alive && clients[i].id == id)
            return i;
    return -1;
}

void raise_client(surface_id_t id) {
    int idx = find_client(id);
    if (idx < 0) return;
    wm_client_t temp = clients[idx];
    for (int i = idx; i + 1 < num_clients; i++) {
        clients[i] = clients[i + 1];
    }
    clients[num_clients - 1] = temp;

    surface_id_t ids[MAX_CLIENTS];
    for (int i = 0; i < num_clients; i++) {
        ids[i] = clients[i].id;
    }
    comp_set_stacking(wm_ch, ids, num_clients);
}

void send_decoration(surface_id_t id) {
    //decoration colors use FB_RGB byte order matching the compositor framebuffer
    comp_decoration_t d = {
        .border_w = BORDER_W,
        .titlebar_h = TITLEBAR_H,
        .tb_focused = FB_RGB(22, 24, 40),
        .tb_unfocused = FB_RGB(14, 15, 24),
        .bd_focused = FB_RGB(96, 104, 224),
        .bd_unfocused = FB_RGB(38, 42, 68),
        .tx_focused = FB_RGB(220, 225, 255),
        .tx_unfocused = FB_RGB(82, 88, 125),
        .close_btn = FB_RGB(210, 60, 60),
        .show_close = true,
    };
    comp_set_decoration(wm_ch, id, d);
}

void remove_client(int idx) {
    if (idx < 0 || idx >= num_clients) return;
    surface_id_t removed_id = clients[idx].id;
    //shift the array to close the gap
    for (int j = idx; j + 1 < num_clients; j++)
        clients[j] = clients[j + 1];
    num_clients--;
    if (focused == removed_id) {
        focused = num_clients ? clients[num_clients - 1].id : 0;
        if (focused != 0)
            comp_set_focus(wm_ch, focused);
    }
    recompute_layout();
}

static void handle_surface_created(comp_msg_t *msg) {
    if (num_clients >= MAX_CLIENTS) return;
    surface_id_t sid = msg->u.surface_created.id;
    INFO("SURFACE_CREATED id=%u pid=%u %ux%u\n", sid, msg->u.surface_created.pid, msg->u.surface_created.w, msg->u.surface_created.h);

    int16 offset = (num_clients) * 30;
    int16 cx = 50 + (offset % 300);
    int16 cy = 50 + (offset % 300);

    wm_client_t c = {
        .id = sid,
        .pid = msg->u.surface_created.pid,
        .w = msg->u.surface_created.w,
        .h = msg->u.surface_created.h,
        .x = cx,
        .y = cy,
        .content_w = msg->u.surface_created.w,
        .content_h = msg->u.surface_created.h,
        .alive = true,
    };
    int k = 0;
    while (k < 31 && msg->u.surface_created.title[k]) { c.title[k] = msg->u.surface_created.title[k]; k++; }
    c.title[k] = '\0';
    clients[num_clients++] = c;

    send_decoration(sid);
    comp_set_position(wm_ch, sid, cx, cy, c.w, c.h);
    raise_client(sid);
    focused = sid;
    comp_set_focus(wm_ch, sid);
}

static void handle_surface_destroyed(comp_msg_t *msg) {
    int idx = find_client(msg->u.surface_destroyed.id);
    if (idx >= 0) remove_client(idx);
}

static void handle_surface_activated(comp_msg_t *msg) {
    surface_id_t sid = msg->u.surface_activated.id;
    focused = sid;
    raise_client(sid);
    comp_set_focus(wm_ch, sid);
}

static void handle_decoration_event(comp_msg_t *msg) {
    surface_id_t sid = msg->u.decoration_event.id;
    //type 0 is the close button
    if (msg->u.decoration_event.type != 0) return;
    int idx = find_client(sid);
    if (idx < 0) return;
    uint32 pid = clients[idx].pid;
    INFO("Killing pid=%u via decoration close\n", pid);
    //tell the compositor to remove the surface immediately - do not wait for
    //the client to send MSG_DESTROY_SURFACE or for its channel to disconnect
    comp_wm_destroy_surface(wm_ch, sid);
    //ask the client process to terminate so it cleans up any other resources
    proc_send_event(pid, PROC_EVENT_TERMINATE);
    remove_client(idx);
}

static void server_listen(void) {
    if (wm_ch == INVALID_HANDLE) return;

    //the WM channel is a flat stream of compositor notifications and key events
    comp_msg_t msg;
    int rc = channel_try_recv(wm_ch, &msg, sizeof(msg));
    if (rc == -3) return;
    if (rc < 0) {
        WARN("WM channel error rc=%d\n", rc);
        return;
    }
    if (rc != (ssize)sizeof(msg)) return;

    switch (msg.type) {
        case MSG_SURFACE_CREATED:   handle_surface_created(&msg); break;
        case MSG_SURFACE_DESTROYED: handle_surface_destroyed(&msg); break;
        case MSG_SURFACE_ACTIVATED: handle_surface_activated(&msg); break;
        case MSG_SURFACE_LIST:
            INFO("SURFACE_LIST count=%u\n", msg.u.surface_list.count);
            break;
        case MSG_KEY_EVENT:         handle_key_event(&msg); break;
        case MSG_DECORATION_EVENT:  handle_decoration_event(&msg); break;
        default:
            WARN("Unknown msg type %d\n", msg.type);
            break;
    }
}

int main(void) {
    //inherit keyboard from the shell via spawn_ctx context, or open from namespace
    if (kbd_init() < 0) {
        WARN("WM: failed to open keyboard channel\n");
    }

    handle_t server_handle = comp_connect();
    ASSERT(server_handle == INVALID_HANDLE, "Failed to connect to compositor\n");

    bool claimed = comp_claim_wm(server_handle, &wm_ch);
    ASSERT(!claimed, "Failed to claim WM\n");
    INFO("WM ready\n");

    while (1) {
        //read raw keyboard events and dispatch them
        kbd_event_t kev;
        while (kbd_try_read(&kev) == 0) {
            comp_msg_t kmsg = { .type = MSG_KEY_EVENT, .u.key_event = { .data = kev, .id = 0 } };
            handle_key_event(&kmsg);
        }
        server_listen();
        yield();
    }

    return 0;
}
