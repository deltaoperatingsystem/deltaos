#ifndef SERVER_H
#define SERVER_H

#include "compositor.h"

//wm focus changes are routed out of band
void notify_wm_surface_activated(surface_id_t id);
//surface channels carry client protocol messages
void handle_client_message(surface_t *s, comp_msg_t *msg);
//wm channel carries policy and layout decisions
void handle_wm_message(comp_msg_t *msg);
//poll the server channel then each live surface channel
void server_listen(void);

#endif
