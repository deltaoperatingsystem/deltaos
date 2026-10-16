#include "server.h"
#include "surface.h"
#include "render.h"

//CONFIGURE is the authoritative size and position message for clients
static void send_configure(surface_t *s) {
    comp_msg_t cfg = {
        .type = MSG_CONFIGURE,
        .u.configure = {
            .id = s->id,
            .x = (uint16)max(s->x, 0),
            .y = (uint16)max(s->y, 0),
            .w = s->content_w,
            .h = s->content_h,
            .bpp = comp.screen_bpp
        }
    };
    send_msg(s->ch, &cfg);
}

static void handle_client_connect(handle_t client_ch, uint32 pid) {
    INFO("Client connecting pid=%u\n", pid);

    if (!comp_ensure_clients(comp.num_clients + 1)) {
        WARN("Out of memory for new client\n");
        //send NACK before closing so the blocked client wakes immediately
        comp_msg_t nack = { .type = MSG_ACK, .u.ack.ok = false };
        send_msg(client_ch, &nack);
        handle_close(client_ch);
        return;
    }

    comp.clients[comp.num_clients] = client_ch;
    comp.client_pids[comp.num_clients] = pid;
    comp.num_clients++;

    comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = true };
    send_msg(client_ch, &ack);
}

static void handle_claim_wm(handle_t reply_ch) {
    if (comp.wm_present) {
        WARN("CLAIM_WM rejected - WM already present\n");
        comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = false };
        send_msg(reply_ch, &ack);
        return;
    }
    static uint32 wm_counter = 0;
    wm_counter++;

    handle_t wm_end = INVALID_HANDLE;
    handle_t client_end = INVALID_HANDLE;
    comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = false };
    if (channel_create(&wm_end, &client_end) == 0 &&
        wm_end != INVALID_HANDLE && client_end != INVALID_HANDLE) {
        comp.wm_ch = wm_end;
        comp.wm_present = true;
        kbd_close();  //WM owns keyboard now; compositor reads via MSG_KEY_EVENT
        INFO("WM claimed\n");

        //send SURFACE_CREATED for each existing surface so the WM can reconstruct state
        for (size_t i = 0; i < comp.num_surfaces; i++) {
            surface_t *s = &comp.surfaces[i];
            if (!s->alive) continue;
            comp_msg_t sc = {
                .type = MSG_SURFACE_CREATED,
                .u.surface_created = { .id = s->id, .pid = s->owner_pid, .w = s->w, .h = s->h, .title = {0} }
            };
            int k = 0;
            while (k < 31 && s->title[k]) { sc.u.surface_created.title[k] = s->title[k]; k++; }
            send_msg(comp.wm_ch, &sc);
        }
        //tell the WM which surface is currently focused
        for (size_t i = 0; i < comp.num_surfaces; i++) {
            if (comp.surfaces[i].alive && comp.surfaces[i].focused) {
                comp_msg_t sa = { .type = MSG_SURFACE_ACTIVATED, .u.surface_activated = { .id = comp.surfaces[i].id } };
                send_msg(comp.wm_ch, &sa);
                break;
            }
        }
        ack.u.ack.ok = true;
        ack.u.ack.id = wm_counter;
        if (channel_send_msg(reply_ch, &ack, sizeof(ack), &client_end, 1) != 0) {
            handle_close(wm_end);
            handle_close(client_end);
            comp.wm_ch = INVALID_HANDLE;
            comp.wm_present = false;
            kbd_init();
            //client blocks on recv_msg, so always reply even on handoff failure
            ack.u.ack.ok = false;
            send_msg(reply_ch, &ack);
        }
        return;
    } else {
        if (wm_end != INVALID_HANDLE) handle_close(wm_end);
        if (client_end != INVALID_HANDLE) handle_close(client_end);
    }
    send_msg(reply_ch, &ack);
}

static void handle_create_surface(handle_t reply_ch, uint32 pid, uint16 w, uint16 h) {
    if (w == 0 || h == 0) {
        WARN("Reject zero-size surface from pid=%u\n", pid);
        comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = false };
        send_msg(reply_ch, &ack);
        return;
    }
    if (!comp_ensure_surfaces(comp.num_surfaces + 1) ||
        !comp_ensure_stack(comp.stack_count + 1)) {
        WARN("Out of memory for surface from pid=%u\n", pid);
        comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = false };
        send_msg(reply_ch, &ack);
        return;
    }

    //create a private channel pair and move the client endpoint in the ACK
    handle_t wm_end = INVALID_HANDLE;
    handle_t client_end = INVALID_HANDLE;
    if (channel_create(&wm_end, &client_end) != 0 ||
        wm_end == INVALID_HANDLE || client_end == INVALID_HANDLE) {
        if (wm_end != INVALID_HANDLE) handle_close(wm_end);
        if (client_end != INVALID_HANDLE) handle_close(client_end);
        comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = false };
        send_msg(reply_ch, &ack);
        return;
    }

    surface_t s = {0};
    surface_create_common(&s, pid, w, h, wm_end);

    if (!s.alive) {
        handle_close(wm_end);
        handle_close(client_end);
        comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = false };
        send_msg(reply_ch, &ack);
        return;
    }

    comp_msg_t ack = { .type = MSG_ACK, .u.ack = { .ok = true, .id = s.id } };
    //compositor retains s.vmo and sends a transferable duplicate to the client
    handle_t client_vmo = handle_dup(s.vmo, RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_WRITE | RIGHT_MAP);
    handle_t handles[2] = { client_end, client_vmo };
    if (client_vmo == INVALID_HANDLE ||
        channel_send_msg(reply_ch, &ack, sizeof(ack), handles, 2) != 0) {
        WARN("Failed to hand off surface id=%u to pid=%u (vmo=%d)\n",
             s.id, pid, client_vmo);
        if (client_end != INVALID_HANDLE) handle_close(client_end);
        if (client_vmo != INVALID_HANDLE) handle_close(client_vmo);
        int idx = find_surface(s.id);
        if (idx >= 0) surface_remove_at(idx);
        ack.u.ack.ok = false;
        send_msg(reply_ch, &ack);
        return;
    }
    send_configure(&s);

    //notify WM about the new surface
    if (comp.wm_present && comp.wm_ch != INVALID_HANDLE && comp.wm_ch != wm_end) {
        comp_msg_t wm_msg = {
            .type = MSG_SURFACE_CREATED,
            .u.surface_created = { .id = s.id, .pid = pid, .w = w, .h = h, .title = {0} }
        };
        send_msg(comp.wm_ch, &wm_msg);
    }

    INFO("Created surface id=%u pid=%u %ux%u\n", s.id, pid, w, h);
}

static void handle_commit(surface_t *s) {
    s->committed = true;
    damage_add_rect(s->x, s->y, s->content_w, s->content_h);
}

static void handle_destroy(surface_id_t id) {
    int idx = find_surface(id);
    if (idx >= 0) {
        damage_add_surface_rect(&comp.surfaces[idx]);
        surface_remove_at(idx);
    }
}

static void handle_resize(surface_t *s, uint16 w, uint16 h) {
    //clients own the VMO size and the compositor just refreshes its view of it
    damage_add_surface_rect(s);
    s->w = w;
    s->h = h;
    if (!surface_map_vmo(s)) {
        return;
    }
    s->committed = true;
    damage_add_surface_rect(s);
    INFO("Resized surface id=%u to %ux%u\n", s->id, w, h);
}

static void handle_set_title(surface_t *s, const char *text) {
    damage_add_surface_rect(s);
    int k = 0;
    while (k < 31 && text[k]) { s->title[k] = text[k]; k++; }
    s->title[k] = '\0';
    damage_add_surface_rect(s);
}

void notify_wm_surface_activated(surface_id_t id) {
    if (!comp.wm_present || comp.wm_ch == INVALID_HANDLE) return;
    comp_msg_t msg = { .type = MSG_SURFACE_ACTIVATED, .u.surface_activated = { .id = id } };
    send_msg(comp.wm_ch, &msg);
}

void handle_client_message(surface_t *s, comp_msg_t *msg) {
    switch (msg->type) {
        case MSG_COMMIT:          handle_commit(s); break;
        case MSG_DESTROY_SURFACE: handle_destroy(s->id); break;
        case MSG_RESIZE_SURFACE:  handle_resize(s, msg->u.resize_surface.w, msg->u.resize_surface.h); break;
        case MSG_SET_TITLE:       handle_set_title(s, msg->u.set_title.text); break;
        default: break;
    }
}

void handle_wm_message(comp_msg_t *msg) {
    switch (msg->type) {
        case MSG_UNCLAIM_WM:
            INFO("WM unclaimed\n");
            handle_close(comp.wm_ch);
            comp.wm_present = false;
            comp.wm_ch = INVALID_HANDLE;
            comp.keyboard_grabbed = false;
            kbd_init();  //reclaim keyboard in fallback mode
            break;

        //the WM can forcibly remove any surface immediately, without waiting
        //for the client process to die and its channel to disconnect
        case MSG_DESTROY_SURFACE:
            handle_destroy(msg->u.destroy_surface.id);
            break;

        case MSG_SET_POSITION: {
            surface_id_t sid = msg->u.set_position.id;
            int idx = find_surface(sid);
            if (idx < 0) break;
            surface_t *s = &comp.surfaces[idx];
            damage_add_surface_rect(s);
            s->x = msg->u.set_position.x;
            s->y = msg->u.set_position.y;
            s->content_w = msg->u.set_position.w;
            s->content_h = msg->u.set_position.h;
            damage_add_surface_rect(s);

            //tell the client its new position and size for buffer reallocation
            send_configure(s);
            break;
        }

        case MSG_SET_FOCUS: {
            int idx = find_surface(msg->u.set_focus.id);
            if (idx < 0) break;
            for (size_t i = 0; i < comp.num_surfaces; i++) {
                if (comp.surfaces[i].focused) {
                    damage_add_surface_rect(&comp.surfaces[i]);
                    comp.surfaces[i].focused = false;
                    if (comp.surfaces[i].ch != INVALID_HANDLE) {
                        comp_msg_t fev = { .type = MSG_FOCUS_EVENT, .u.focus_event = { .id = comp.surfaces[i].id, .focused = false } };
                        send_msg(comp.surfaces[i].ch, &fev);
                    }
                }
            }
            comp.surfaces[idx].focused = true;
            damage_add_surface_rect(&comp.surfaces[idx]);
            comp_msg_t ev = { .type = MSG_FOCUS_EVENT, .u.focus_event = { .id = comp.surfaces[idx].id, .focused = true } };
            send_msg(comp.surfaces[idx].ch, &ev);
            break;
        }

        case MSG_SET_DECORATION: {
            int idx = find_surface(msg->u.set_decoration.id);
            if (idx < 0) break;
            damage_add_surface_rect(&comp.surfaces[idx]);
            comp.surfaces[idx].deco = msg->u.set_decoration.d;
            damage_add_surface_rect(&comp.surfaces[idx]);
            break;
        }

        case MSG_SET_STACKING: {
            size_t n = msg->u.set_stacking.count;
            size_t offset = msg->u.set_stacking.offset;
            size_t total = msg->u.set_stacking.total;
            if (offset > total || n > total - offset || !comp_ensure_stack(total)) break;
            if (offset == 0) comp.stack_count = total;
            if (comp.stack_count != total) break;
            for (size_t i = 0; i < n; i++)
                comp.stack[offset + i] = msg->u.set_stacking.ids[i];
            damage_add_rect(0, 0, comp.screen_w, comp.screen_h);
            break;
        }

        case MSG_SET_KEYBOARD_GRAB:
            comp.keyboard_grabbed = msg->u.set_keyboard_grab.grab;
            break;

        case MSG_PASS_THROUGH: {
            int idx = find_surface(msg->u.pass_through.id);
            if (idx >= 0 && !comp.keyboard_grabbed) {
                comp_msg_t kev = { .type = MSG_KEY_EVENT, .u.key_event = { .data = msg->u.pass_through.data, .id = comp.surfaces[idx].id } };
                send_msg(comp.surfaces[idx].ch, &kev);
            }
            break;
        }

        case MSG_SET_CLIENT_AREA: {
            int idx = find_surface(msg->u.set_client_area.id);
            if (idx >= 0) {
                damage_add_surface_rect(&comp.surfaces[idx]);
                comp.surfaces[idx].content_x = msg->u.set_client_area.x;
                comp.surfaces[idx].content_y = msg->u.set_client_area.y;
                comp.surfaces[idx].content_w = msg->u.set_client_area.w;
                comp.surfaces[idx].content_h = msg->u.set_client_area.h;
                damage_add_surface_rect(&comp.surfaces[idx]);
            }
            break;
        }

        default:
            WARN("Unknown WM msg type %d\n", msg->type);
            break;
    }
}

void server_listen(void) {
    comp_msg_t msg;
    uint32 pid = 0;

    //read the server channel for new connections
    handle_t client_ch = INVALID_HANDLE;
    channel_recv_result_t recv_result;
    int rc = channel_try_recv_msg(comp.server_handle, &msg, sizeof(msg),
                                  &client_ch, 1, &recv_result);
    if (rc == 0) {
        if (recv_result.data_len != sizeof(msg)) {
            if (client_ch != INVALID_HANDLE) handle_close(client_ch);
            WARN("Invalid bootstrap message size %u\n", (uint32)recv_result.data_len);
        } else if (msg.type == MSG_CLIENT_CONNECT && recv_result.handle_count == 1 &&
                   client_ch != INVALID_HANDLE) {
            handle_client_connect(client_ch, recv_result.sender_pid);
        } else {
            if (client_ch != INVALID_HANDLE) handle_close(client_ch);
            WARN("Invalid bootstrap message type=%d handles=%u from pid=%u\n",
                 msg.type, recv_result.handle_count, recv_result.sender_pid);
        }
    } else if (rc < 0 && rc != -3) {
        WARN("Server bootstrap recv error %d\n", rc);
    }

    //poll dedicated client channels
    for (size_t i = 0; i < comp.num_clients; i++) {
        handle_t ch = comp.clients[i];
        uint32 client_pid = comp.client_pids[i];
        rc = recv_msg(ch, &msg, &pid);
        if (rc == -3) continue;
        if (rc < 0) {
            INFO("Client pid=%u disconnected from dedicated channel\n", client_pid);
            handle_close(ch);
            for (size_t j = i; j + 1 < comp.num_clients; j++) {
                comp.clients[j] = comp.clients[j + 1];
                comp.client_pids[j] = comp.client_pids[j + 1];
            }
            comp.num_clients--;
            i--;
            continue;
        }

        switch (msg.type) {
            case MSG_CLIENT_CONNECT: {
                comp_msg_t ack = { .type = MSG_ACK, .u.ack.ok = true };
                send_msg(ch, &ack);
                break;
            }
            case MSG_CREATE_SURFACE:
                handle_create_surface(ch, client_pid, msg.u.create_surface.w, msg.u.create_surface.h);
                break;
            case MSG_CLAIM_WM:
                handle_claim_wm(ch);
                break;
            default:
                WARN("Unexpected msg type %d on dedicated channel from pid=%u\n", msg.type, client_pid);
                break;
        }
    }

    //surface channels are polled after the global server channel each frame
    for (size_t i = 0; i < comp.num_surfaces; i++) {
        surface_t *s = &comp.surfaces[i];
        if (!s->alive || s->ch == INVALID_HANDLE) continue;

        rc = recv_msg(s->ch, &msg, &pid);
        if (rc == -3) continue;
        if (rc < 0) {
            WARN("Client pid=%u surface id=%u disconnected (rc=%d)\n", s->owner_pid, s->id, rc);
            surface_remove_at(i--);
            continue;
        }

        if (s->ch == comp.wm_ch) {
            handle_wm_message(&msg);
        } else {
            handle_client_message(s, &msg);
        }
    }

    //also check the WM channel directly when the WM has no surface
    if (comp.wm_present && comp.wm_ch != INVALID_HANDLE) {
        int wm_surf_idx = find_surface_by_ch(comp.wm_ch);
        if (wm_surf_idx < 0) {
            rc = recv_msg(comp.wm_ch, &msg, &pid);
            if (rc == 0) {
                handle_wm_message(&msg);
            } else if (rc < 0 && rc != -3) {
                WARN("WM channel error rc=%d\n", rc);
                handle_close(comp.wm_ch);
                comp.wm_present = false;
                comp.wm_ch = INVALID_HANDLE;
                comp.keyboard_grabbed = false;
                kbd_init();  //reclaim keyboard since WM is gone
            }
        }
    }
}
