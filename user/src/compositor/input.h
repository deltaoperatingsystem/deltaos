#ifndef INPUT_H
#define INPUT_H

#include "compositor.h"

//returns 0 for miss 1 for body 2 for close button
int hit_test(int mx, int my, surface_id_t *out_sid);
//drains keyboard and mouse devices once per compositor frame
void handle_input(void);

#endif
