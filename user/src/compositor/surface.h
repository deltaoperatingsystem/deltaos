#ifndef SURFACE_H
#define SURFACE_H

#include "compositor.h"

//lookup helpers for id based and channel based routing
int find_surface(surface_id_t id);
int find_surface_by_ch(handle_t ch);
//removal closes handles and compacts arrays
void surface_remove_at(int idx);
//remap compositor side pixels after the client resized the VMO
bool surface_map_vmo(surface_t *s);
//shared creation path for app surfaces and future internal surfaces
void surface_create_common(surface_t *s, uint32 pid, uint16 w, uint16 h, handle_t ch);

#endif
