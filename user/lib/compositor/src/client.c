#include <compositor/client.h>
#include <system.h>
#include <io.h>
#include <string.h>

handle_t comp_connect(void) {
    //create dedicated channel pair
    handle_t client_end = INVALID_HANDLE;
    handle_t server_end = INVALID_HANDLE;
    if (channel_create(&client_end, &server_end) != 0) {
        return INVALID_HANDLE;
    }

    //open bootstrap channel
    handle_t bootstrap_ch = INVALID_HANDLE;
    for (int i = 0; i < 50; i++) {
        bootstrap_ch = get_obj(INVALID_HANDLE, "$gui/display/server", RIGHT_READ | RIGHT_WRITE);
        if (bootstrap_ch != INVALID_HANDLE) break;
        yield();
    }
    if (bootstrap_ch == INVALID_HANDLE) {
        handle_close(client_end);
        handle_close(server_end);
        return INVALID_HANDLE;
    }

    //move the compositor endpoint to the server over the bootstrap channel
    comp_msg_t msg = { .type = MSG_CLIENT_CONNECT };
    msg.u.client_connect.pid = (uint32)getpid();
    msg.u.client_connect.conn_id = 0;
    if (channel_send_msg(bootstrap_ch, &msg, sizeof(msg), &server_end, 1) != 0) {
        handle_close(bootstrap_ch);
        handle_close(client_end);
        handle_close(server_end);
        return INVALID_HANDLE;
    }
    handle_close(bootstrap_ch);

    //wait for ACK on our private client_end channel
    comp_msg_t resp;
    int rc = channel_recv(client_end, &resp, sizeof(resp));
    if (rc != (ssize)sizeof(resp) || resp.type != MSG_ACK || !resp.u.ack.ok) {
        handle_close(client_end);
        return INVALID_HANDLE;
    }

    return client_end;
}

bool comp_claim_wm(handle_t server_ch, handle_t *out_wm_ch) {
    if (server_ch == INVALID_HANDLE || !out_wm_ch) return false;

    comp_msg_t msg = { .type = MSG_CLAIM_WM };
    if (channel_send(server_ch, &msg, sizeof(msg)) != 0) return false;

    comp_msg_t resp;
    handle_t wm_ch = INVALID_HANDLE;
    channel_recv_result_t result;
    int rc = channel_recv_msg(server_ch, &resp, sizeof(resp), &wm_ch, 1, &result);
    if (rc != 0 || result.data_len != sizeof(resp) || result.handle_count != 1 ||
        resp.type != MSG_ACK || !resp.u.ack.ok || wm_ch == INVALID_HANDLE) {
        if (wm_ch != INVALID_HANDLE) handle_close(wm_ch);
        return false;
    }
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

bool comp_set_stacking(handle_t wm_ch, const surface_id_t *ids, size count) {
    if (wm_ch == INVALID_HANDLE || (!ids && count > 0) || count > UINT32_MAX) return false;

    for (size offset = 0; offset < count || (count == 0 && offset == 0); offset += 256) {
        uint16 chunk = (uint16)((count - offset) > 256 ? 256 : (count - offset));
        comp_msg_t msg = {
            .type = MSG_SET_STACKING,
            .u.set_stacking = {
                .count = chunk,
                .offset = (uint32)offset,
                .total = (uint32)count,
            }
        };
        if (chunk > 0) memcpy(msg.u.set_stacking.ids, ids + offset, chunk * sizeof(surface_id_t));
        if (channel_send(wm_ch, &msg, sizeof(msg)) != 0) return false;
        if (count == 0) break;
    }
    return true;
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

bool comp_create_surface(handle_t server_ch, uint16 w, uint16 h, surface_id_t *out_id,
                         handle_t *out_ch, handle_t *out_vmo) {
    if (server_ch == INVALID_HANDLE || !out_id || !out_ch || !out_vmo) return false;

    comp_msg_t req = {
        .type = MSG_CREATE_SURFACE,
        .u.create_surface = { .w = w, .h = h }
    };
    if (channel_send(server_ch, &req, sizeof(req)) != 0) return false;

    comp_msg_t ack = {0};
    handle_t handles[2] = { INVALID_HANDLE, INVALID_HANDLE };
    channel_recv_result_t result;
    int rc = channel_recv_msg(server_ch, &ack, sizeof(ack), handles, 2, &result);
    if (rc != 0 || result.data_len != sizeof(ack) || result.handle_count != 2 ||
        ack.type != MSG_ACK || !ack.u.ack.ok ||
        handles[0] == INVALID_HANDLE || handles[1] == INVALID_HANDLE) {
        if (handles[0] != INVALID_HANDLE) handle_close(handles[0]);
        if (handles[1] != INVALID_HANDLE) handle_close(handles[1]);
        return false;
    }

    *out_id = ack.u.ack.id;
    *out_ch = handles[0];
    *out_vmo = handles[1];
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
