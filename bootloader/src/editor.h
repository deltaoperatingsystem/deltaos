#ifndef _EDITOR_H
#define _EDITOR_H

#include <stdint.h>
#include "config.h"

#define EDITOR_MAX_FIELDS 3

//return values from editor_run
#define EDITOR_BOOT     1   //boot with edited values
#define EDITOR_CANCEL   0   //cancel and return to menu

//field indices
#define EDITOR_FIELD_PATH    0
#define EDITOR_FIELD_INITRD  1
#define EDITOR_FIELD_CMDLINE 2

typedef struct {
    int field_index;        //currently selected field
    int cursor_pos;         //cursor position within field
    char *fields[EDITOR_MAX_FIELDS]; //dynamically allocated editable copies
    char *name;             //dynamically allocated entry name (read-only)
} EditorState;

//run the editor for a config entry
//returns EDITOR_BOOT or EDITOR_CANCEL
//if EDITOR_BOOT, the entry's path/initrd/cmdline are updated with edited values
int editor_run(ConfigEntry *entry);

#endif
