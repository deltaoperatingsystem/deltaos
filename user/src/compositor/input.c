#include "input.h"
#include "server.h"
#include "surface.h"
#include "render.h"

int hit_test(int mx, int my, surface_id_t *out_sid) {
    for (int si = comp.stack_count - 1; si >= 0; si--) {
        int idx = find_surface(comp.stack[si]);
        if (idx < 0) continue;
        surface_t *s = &comp.surfaces[idx];
        if (!s->alive || !s->committed) continue;

        int dx = (int)s->x - (int)s->deco.border_w;
        int dy = (int)s->y - (int)s->deco.titlebar_h - (int)s->deco.border_w;
        int dw = (int)s->content_w + 2 * (int)s->deco.border_w;
        int dh = (int)s->content_h + (int)s->deco.titlebar_h + (int)s->deco.border_w;

        if (mx >= dx && my >= dy && mx < dx + dw && my < dy + dh) {
            if (s->deco.show_close) {
                int btn = 10;
                int bx = dx + dw - s->deco.border_w - btn - 4;
                int by = dy + (s->deco.titlebar_h - btn) / 2;
                if (mx >= bx && my >= by && mx < bx + btn && my < by + btn) {
                    *out_sid = s->id;
                    return 2;
                }
            }
            if (my < s->y) {
                *out_sid = s->id;
                return 3;
            }
            *out_sid = s->id;
            return 1;
        }
    }
    return 0;
}

void handle_input(void) {
    //only read raw keyboard when no WM is active; when a WM is present it owns
    //the keyboard channel and forwards key events to us via MSG_KEY_EVENT
    if (!comp.wm_present) {
        kbd_event_t ev;
        if (kbd_try_read(&ev) == 0) {
            //no WM to forward to in fallback mode - drop the event
            (void)ev;
        }
    }

    //lazy init the mouse channel
    if (comp.mouse_h == INVALID_HANDLE)
        comp.mouse_h = get_obj(INVALID_HANDLE, "$devices/mouse/channel", RIGHT_READ);

    mouse_event_t m;
    while (comp.mouse_h != INVALID_HANDLE &&
           channel_try_recv(comp.mouse_h, &m, sizeof(m)) == (ssize)sizeof(m)) {
        //mouse packets are relative so the compositor owns the absolute cursor state
        int32 old_mx = comp.mouse_x;
        int32 old_my = comp.mouse_y;
        comp.mouse_x += m.dx;
        comp.mouse_y += m.dy;
        if (comp.mouse_x < 0) comp.mouse_x = 0;
        if (comp.mouse_y < 0) comp.mouse_y = 0;
        if (comp.mouse_x >= (int32)comp.screen_w) comp.mouse_x = comp.screen_w - 1;
        if (comp.mouse_y >= (int32)comp.screen_h) comp.mouse_y = comp.screen_h - 1;

        if (comp.mouse_x != old_mx || comp.mouse_y != old_my) {
            damage_add_rect(old_mx, old_my, cursor_get_width(), cursor_get_height());
            damage_add_rect(comp.mouse_x, comp.mouse_y, cursor_get_width(), cursor_get_height());
        }

        if (!(m.buttons & MOUSE_BTN_LEFT)) {
            comp.is_dragging = false;
        }

        if (m.buttons & MOUSE_BTN_LEFT && !(comp.mprev.buttons & MOUSE_BTN_LEFT)) {
            surface_id_t hit_id = 0;
            int hit = hit_test(comp.mouse_x, comp.mouse_y, &hit_id);

            if (hit == 2 && comp.wm_present && comp.wm_ch != INVALID_HANDLE) {
                comp_msg_t msg = { .type = MSG_DECORATION_EVENT, .u.decoration_event = { .id = hit_id, .type = 0 } };
                send_msg(comp.wm_ch, &msg);
            } else if (hit == 1 || hit == 3) {
                int idx = find_surface(hit_id);
                if (idx >= 0) {
                    for (int i = 0; i < comp.num_surfaces; i++) {
                        if (comp.surfaces[i].focused) {
                            damage_add_surface_rect(&comp.surfaces[i]);
                            comp.surfaces[i].focused = false;
                        }
                    }
                    comp.surfaces[idx].focused = true;
                    damage_add_surface_rect(&comp.surfaces[idx]);
                    comp_msg_t fev = { .type = MSG_FOCUS_EVENT, .u.focus_event = { .id = hit_id, .focused = true } };
                    send_msg(comp.surfaces[idx].ch, &fev);
                    notify_wm_surface_activated(hit_id);

                    if (hit == 3) {
                        comp.is_dragging = true;
                        comp.drag_sid = hit_id;
                        comp.drag_offset_x = (int16)(comp.mouse_x - comp.surfaces[idx].x);
                        comp.drag_offset_y = (int16)(comp.mouse_y - comp.surfaces[idx].y);
                    } else {
                        comp_msg_t mev = {
                            .type = MSG_MOUSE_EVENT,
                            .u.mouse_event = {
                                .id = hit_id,
                                .x = (int16)(comp.mouse_x - comp.surfaces[idx].x),
                                .y = (int16)(comp.mouse_y - comp.surfaces[idx].y),
                                .buttons = m.buttons
                            }
                        };
                        send_msg(comp.surfaces[idx].ch, &mev);
                    }
                }
            }
        } else if (m.buttons & MOUSE_BTN_LEFT) {
            if (comp.is_dragging) {
                int idx = find_surface(comp.drag_sid);
                if (idx >= 0) {
                    surface_t *s = &comp.surfaces[idx];
                    int16 new_x = (int16)(comp.mouse_x - comp.drag_offset_x);
                    int16 new_y = (int16)(comp.mouse_y - comp.drag_offset_y);
                    int16 min_y = (int16)(s->deco.titlebar_h + s->deco.border_w);
                    if (new_y < min_y) new_y = min_y;
                    if (s->x != new_x || s->y != new_y) {
                        damage_add_surface_rect(s);
                        s->x = new_x;
                        s->y = new_y;
                        damage_add_surface_rect(s);
                    }
                }
            } else {
                for (int i = 0; i < comp.num_surfaces; i++) {
                    if (comp.surfaces[i].focused && comp.surfaces[i].ch != INVALID_HANDLE) {
                        comp_msg_t mev = {
                            .type = MSG_MOUSE_EVENT,
                            .u.mouse_event = {
                                .id = comp.surfaces[i].id,
                                .x = (int16)(comp.mouse_x - comp.surfaces[i].x),
                                .y = (int16)(comp.mouse_y - comp.surfaces[i].y),
                                .buttons = m.buttons
                            }
                        };
                        send_msg(comp.surfaces[i].ch, &mev);
                        break;
                    }
                }
            }
        }
        comp.mprev = m;
    }
}
