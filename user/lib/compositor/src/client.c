#include <compositor/client.h>
#include <system.h>
#include <io.h>
#include <string.h>

handle_t comp_connect(void) {
    handle_t h = INVALID_HANDLE;
    for (int i = 0; i < 50; i++) {
        h = get_obj(INVALID_HANDLE, "$gui/display/server", RIGHT_READ | RIGHT_WRITE);
        if (h != INVALID_HANDLE) break;
        yield();
    }
    return h;
}

bool comp_claim_wm(handle_t server_ch, handle_t *out_wm_ch) {
    if (server_ch == INVALID_HANDLE || !out_wm_ch) return false;

    comp_msg_t msg = { .type = MSG_CLAIM_WM };
    if (channel_send(server_ch, &msg, sizeof(msg)) != 0) return false;

    comp_msg_t resp;
    bool got_ack = false;
    for (int i = 0; i < 100; i++) {
        int rc = channel_try_recv(server_ch, &resp, sizeof(resp));
        if (rc == (ssize)sizeof(resp) && resp.type == MSG_ACK && resp.u.ack.ok) {
            got_ack = true;
            break;
        }
        yield();
    }
    if (!got_ack) return false;

    char path[64];
    snprintf(path, sizeof(path), "$gui/display/%u_wm/ch", getpid());

    handle_t wm_ch = INVALID_HANDLE;
    for (int i = 0; i < 100; i++) {
        wm_ch = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_WRITE);
        if (wm_ch != INVALID_HANDLE) break;
        yield();
    }

    if (wm_ch == INVALID_HANDLE) return false;
    *out_wm_ch = wm_ch;
    return true;
}

void comp_unclaim_wm(handle_t wm_ch) {
    if (wm_ch == INVALID_HANDLE) return;
    comp_msg_t msg = { .type = MSG_UNCLAIM_WM };
    channel_send(wm_ch, &msg, sizeof(msg));
}

bool comp_set_position(handle_t wm_ch, surface_id_t id, int16 x, int16 y, uint16 w, uint16 h) {
    if (wm_ch == INVALID_HANDLE) return false;
    comp_msg_t msg = {
        .type = MSG_SET_POSITION,
        .u.set_position = { .id = id, .x = x, .y = y, .w = w, .h = h }
    };
    return channel_send(wm_ch, &msg, sizeof(msg)) == 0;
}

bool comp_set_focus(handle_t ch, surface_id_t id) {
    if (ch == INVALID_HANDLE) return false;
    comp_msg_t msg = {
        .type = MSG_SET_FOCUS,
        .u.set_focus = { .id = id }
    };
    return channel_send(ch, &msg, sizeof(msg)) == 0;
}

bool comp_set_decoration(handle_t wm_ch, surface_id_t id, comp_decoration_t d) {
    if (wm_ch == INVALID_HANDLE) return false;
    comp_msg_t msg = {
        .type = MSG_SET_DECORATION,
        .u.set_decoration = { .id = id, .d = d }
    };
    return channel_send(wm_ch, &msg, sizeof(msg)) == 0;
}

bool comp_set_stacking(handle_t wm_ch, const surface_id_t *ids, uint8 count) {
    if (wm_ch == INVALID_HANDLE || !ids || count > 32) return false;
    comp_msg_t msg = {
        .type = MSG_SET_STACKING,
        .u.set_stacking = { .count = count }
    };
    memcpy(msg.u.set_stacking.ids, ids, count * sizeof(surface_id_t));
    return channel_send(wm_ch, &msg, sizeof(msg)) == 0;
}

bool comp_set_client_area(handle_t wm_ch, surface_id_t id, uint16 x, uint16 y, uint16 w, uint16 h) {
    if (wm_ch == INVALID_HANDLE) return false;
    comp_msg_t msg = {
        .type = MSG_SET_CLIENT_AREA,
        .u.set_client_area = { .id = id, .x = x, .y = y, .w = w, .h = h }
    };
    return channel_send(wm_ch, &msg, sizeof(msg)) == 0;
}

bool comp_pass_through(handle_t wm_ch, surface_id_t id, kbd_event_t ev) {
    if (wm_ch == INVALID_HANDLE) return false;
    comp_msg_t msg = {
        .type = MSG_PASS_THROUGH,
        .u.pass_through = { .id = id, .data = ev }
    };
    return channel_send(wm_ch, &msg, sizeof(msg)) == 0;
}

//sk the compositor to immediately remove a surface on the WM channel
//this is used when the WM kills a client - it does not wait for the client
//process to die and its channel to disconnect on its own
bool comp_wm_destroy_surface(handle_t wm_ch, surface_id_t id) {
    if (wm_ch == INVALID_HANDLE) return false;
    comp_msg_t msg = {
        .type = MSG_DESTROY_SURFACE,
        .u.destroy_surface = { .id = id }
    };
    return channel_send(wm_ch, &msg, sizeof(msg)) == 0;
}

bool comp_create_surface(handle_t server_ch, uint16 w, uint16 h, surface_id_t *out_id, handle_t *out_ch) {
    if (server_ch == INVALID_HANDLE || !out_id || !out_ch) return false;

    comp_msg_t req = {
        .type = MSG_CREATE_SURFACE,
        .u.create_surface = { .w = w, .h = h }
    };
    if (channel_send(server_ch, &req, sizeof(req)) != 0) return false;

    comp_msg_t ack = {0};
    bool got_ack = false;
    for (int i = 0; i < 50; i++) {
        int rc = channel_try_recv(server_ch, &ack, sizeof(ack));
        if (rc == (ssize)sizeof(ack) && ack.type == MSG_ACK) {
            got_ack = ack.u.ack.ok;
            break;
        }
        yield();
    }
    if (!got_ack) return false;

    char path[64];
    snprintf(path, sizeof(path), "$gui/display/%u_%u/ch", getpid(), ack.u.ack.id);

    handle_t ch = INVALID_HANDLE;
    for (int i = 0; i < 50; i++) {
        ch = get_obj(INVALID_HANDLE, path, RIGHT_READ | RIGHT_WRITE);
        if (ch != INVALID_HANDLE) break;
        yield();
    }
    if (ch == INVALID_HANDLE) {
        comp_msg_t cleanup_msg = {
            .type = MSG_DESTROY_SURFACE,
            .u.destroy_surface.id = ack.u.ack.id
        };
        channel_send(server_ch, &cleanup_msg, sizeof(cleanup_msg));
        return false;
    }

    *out_id = ack.u.ack.id;
    *out_ch = ch;
    return true;
}

void comp_commit(handle_t surface_ch, surface_id_t id) {
    if (surface_ch == INVALID_HANDLE) return;
    comp_msg_t msg = { .type = MSG_COMMIT, .u.commit.id = id };
    channel_send(surface_ch, &msg, sizeof(msg));
}

void comp_destroy_surface(handle_t surface_ch, surface_id_t id) {
    if (surface_ch == INVALID_HANDLE) return;
    comp_msg_t msg = { .type = MSG_DESTROY_SURFACE, .u.destroy_surface.id = id };
    channel_send(surface_ch, &msg, sizeof(msg));
}

void comp_resize_surface(handle_t surface_ch, surface_id_t id, uint16 w, uint16 h) {
    if (surface_ch == INVALID_HANDLE) return;
    comp_msg_t msg = {
        .type = MSG_RESIZE_SURFACE,
        .u.resize_surface = { .id = id, .w = w, .h = h }
    };
    channel_send(surface_ch, &msg, sizeof(msg));
}

void comp_set_title(handle_t surface_ch, surface_id_t id, const char *title) {
    if (surface_ch == INVALID_HANDLE || !title) return;
    comp_msg_t msg = { .type = MSG_SET_TITLE, .u.set_title = { .id = id } };
    int k = 0;
    while (k < 31 && title[k]) {
        msg.u.set_title.text[k] = title[k];
        k++;
    }
    msg.u.set_title.text[k] = '\0';
    channel_send(surface_ch, &msg, sizeof(msg));
}
