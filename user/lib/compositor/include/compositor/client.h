#ifndef COMPOSITOR_CLIENT_H
#define COMPOSITOR_CLIENT_H

#include <system.h>
#include <compositor/protocol.h>

//client connection
handle_t comp_connect(void);
bool comp_claim_wm(handle_t server_ch, handle_t *out_wm_ch);
void comp_unclaim_wm(handle_t wm_ch);

//window manager operations
bool comp_set_position(handle_t wm_ch, surface_id_t id, int16 x, int16 y, uint16 w, uint16 h);
bool comp_set_focus(handle_t ch, surface_id_t id);
bool comp_set_decoration(handle_t wm_ch, surface_id_t id, comp_decoration_t d);
bool comp_set_stacking(handle_t wm_ch, const surface_id_t *ids, uint8 count);
bool comp_set_client_area(handle_t wm_ch, surface_id_t id, uint16 x, uint16 y, uint16 w, uint16 h);
bool comp_pass_through(handle_t wm_ch, surface_id_t id, kbd_event_t ev);
bool comp_wm_destroy_surface(handle_t wm_ch, surface_id_t id);

//client operations
bool comp_create_surface(handle_t server_ch, uint16 w, uint16 h, surface_id_t *out_id, handle_t *out_ch);
void comp_commit(handle_t surface_ch, surface_id_t id);
void comp_destroy_surface(handle_t surface_ch, surface_id_t id);
void comp_resize_surface(handle_t surface_ch, surface_id_t id, uint16 w, uint16 h);
void comp_set_title(handle_t surface_ch, surface_id_t id, const char *title);

#endif
