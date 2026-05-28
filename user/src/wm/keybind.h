#ifndef KEYBIND_H
#define KEYBIND_H

#include "wm.h"

//handles WM only shortcuts then forwards anything unclaimed
void handle_key_event(comp_msg_t *msg);

#endif
