#ifndef _MENU_H
#define _MENU_H

#include <stdint.h>

#define MENU_MAX_ENTRIES 16

typedef struct {
    const char *name;
    const char *path;
} MenuEntry;

void menu_init(void);
int menu_add_entry(const char *name, const char *path);
int menu_run(int timeout, int default_index);
MenuEntry *menu_get_entry(int index);

#endif
