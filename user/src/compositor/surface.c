#include "surface.h"
#include "render.h"

//stack[] controls z order and surfaces[] is the allocation pool
int find_surface(surface_id_t id) {
    for (int i = 0; i < comp.num_surfaces; i++)
        if (comp.surfaces[i].alive && comp.surfaces[i].id == id)
            return i;
    return -1;
}

//needed when the WM has no surface yet but does have a channel
int find_surface_by_ch(handle_t ch) {
    for (int i = 0; i < comp.num_surfaces; i++)
        if (comp.surfaces[i].alive && comp.surfaces[i].ch == ch)
            return i;
    return -1;
}

void surface_remove_at(int idx) {
    if (idx < 0 || idx >= comp.num_surfaces) return;
    surface_t *s = &comp.surfaces[idx];
    surface_id_t removed_id = s->id;
    INFO("Remove surface id=%u pid=%u\n", s->id, s->owner_pid);
    damage_add_surface_rect(s);

    //tear down the VMO mapping VMO handle and channel
    if (s->pixels && s->vmo_size > 0)
        vmo_unmap(s->pixels, s->vmo_size);
    if (s->vmo != INVALID_HANDLE)
        handle_close(s->vmo);
    if (s->ch != INVALID_HANDLE)
        handle_close(s->ch);

    //notify the WM so it can update its client list
    if (comp.wm_present && comp.wm_ch != INVALID_HANDLE) {
        comp_msg_t msg = { .type = MSG_SURFACE_DESTROYED, .u.surface_destroyed = { .id = s->id } };
        send_msg(comp.wm_ch, &msg);
    }

    //shift the surfaces array to close the gap
    for (int j = idx; j + 1 < comp.num_surfaces; j++)
        comp.surfaces[j] = comp.surfaces[j + 1];
    comp.num_surfaces--;

    //remove from the stack too
    uint8 sc = 0;
    for (int j = 0; j < comp.stack_count; j++)
        if (comp.stack[j] != removed_id)
            comp.stack[sc++] = comp.stack[j];
    comp.stack_count = sc;
}

bool surface_map_vmo(surface_t *s) {
    //the client already resized the underlying VMO before this remap
    //unmap the old view if any then remap at the new size
    if (s->pixels) {
        vmo_unmap(s->pixels, s->vmo_size);
        s->pixels = NULL;
    }
    s->vmo_size = (size)s->w * s->h * comp.screen_bpp;
    s->pixels = vmo_map(s->vmo, NULL, 0, s->vmo_size, RIGHT_MAP);
    if (!s->pixels) {
        ERROR("vmo_map failed for surface id=%u\n", s->id);
        return false;
    }
    return true;
}

void surface_create_common(surface_t *s, uint32 pid, uint16 w, uint16 h, handle_t ch) {
    //surface buffers are shared VMOs so clients and the compositor see the same pixels
    size needed = (size)w * h * comp.screen_bpp;
    handle_t vmo = vmo_create(needed, VMO_FLAG_RESIZABLE, RIGHT_WRITE | RIGHT_MAP);
    if (vmo == INVALID_HANDLE) return;

    uint32 *pixels = vmo_map(vmo, NULL, 0, needed, RIGHT_MAP);
    if (!pixels) { handle_close(vmo); return; }

    s->id = comp.next_id++;
    s->owner_pid = pid;
    s->ch = ch;
    s->vmo = vmo;
    s->vmo_size = needed;
    s->pixels = pixels;
    s->w = w;
    s->h = h;
    s->alive = true;
    s->committed = false;
    //default decoration colors
    s->deco.border_w = BORDER_W;
    s->deco.titlebar_h = TITLEBAR_H;
    s->deco.tb_focused = DECO_TB_FOCUSED;
    s->deco.tb_unfocused = DECO_TB_UNFOCUSED;
    s->deco.bd_focused = DECO_BD_FOCUSED;
    s->deco.bd_unfocused = DECO_BD_UNFOCUSED;
    s->deco.tx_focused = DECO_TX_FOCUSED;
    s->deco.tx_unfocused = DECO_TX_UNFOCUSED;
    s->deco.close_btn = DECO_CLOSE_BTN;
    s->deco.show_close = true;

    //default position and visible size WM may override via SET_POSITION
    s->x = 40;
    s->y = 40;
    s->content_w = w;
    s->content_h = h;

    //register the VMO at the PID specific namespace path for the client to map
    //ceiling: clients may only write pixels into it and map it - nothing else
    char path[64];
    snprintf(path, sizeof(path), "$gui/display/%u_%u/surface", pid, s->id);
    if (ns_register(path, vmo, RIGHT_WRITE | RIGHT_MAP) != 0) {
        vmo_unmap(s->pixels, s->vmo_size);
        handle_close(vmo);
        s->alive = false;
        s->pixels = NULL;
        s->vmo = INVALID_HANDLE;
        return;
    }

    comp.surfaces[comp.num_surfaces++] = *s;
    comp.stack[comp.stack_count++] = s->id;
}
