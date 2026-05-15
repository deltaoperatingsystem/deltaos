#include "editor.h"
#include "graphics.h"
#include "console.h"
#include "efi.h"
#include "stdio.h"
#include <string.h>

//set by main.c
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

//UEFI scan codes for special keys
#define SCAN_UP      0x01
#define SCAN_DOWN    0x02
#define SCAN_RIGHT   0x03
#define SCAN_LEFT    0x04
#define SCAN_HOME    0x05
#define SCAN_END     0x06
#define SCAN_INSERT  0x07
#define SCAN_DELETE  0x08
#define SCAN_F10     0x14
#define SCAN_ESCAPE  0x17

//display constants
#define FIELD_LABEL_WIDTH  10  //characters for "Path:     " etc
#define FIELD_MAX_VISIBLE  40  //max visible chars in field
#define BOX_PADDING        2   //pixels padding inside box

static const char *field_labels[EDITOR_MAX_FIELDS] = {
    "Path:",
    "Initrd:",
    "Cmdline:"
};

//draw a single field with label and editable content
static void draw_field(EditorState *state, int index, int is_selected) {
    uint32_t char_h = con_get_char_height();
    uint32_t char_w = con_get_char_width();

    //calculate position
    uint32_t field_y = char_h * (4 + index * 3);
    uint32_t label_x = char_w * 2;
    uint32_t field_x = char_w * (FIELD_LABEL_WIDTH + 2);

    //draw label
    con_set_color(COLOR_GRAY, 0);
    con_print_at(label_x, field_y, field_labels[index]);

    //draw field background box
    uint32_t box_x = field_x - BOX_PADDING;
    uint32_t box_y = field_y - BOX_PADDING;
    uint32_t box_w = (FIELD_MAX_VISIBLE + 2) * char_w + BOX_PADDING * 2;
    uint32_t box_h = char_h + BOX_PADDING * 2;

    if (is_selected) {
        //selected: slightly lighter background
        gfx_fill_rect(box_x, box_y, box_w, box_h, 0x00181818);
        gfx_draw_rect(box_x, box_y, box_w, box_h, COLOR_HIGHLIGHT);
    } else {
        //not selected: dark background
        gfx_fill_rect(box_x, box_y, box_w, box_h, 0x00101010);
        gfx_draw_rect(box_x, box_y, box_w, box_h, COLOR_GRAY);
    }

    //calculate visible portion of field (scrolling)
    const char *text = state->fields[index];
    int len = strlen(text);
    int visible_start = 0;

    //scroll to keep cursor visible
    if (state->cursor_pos > FIELD_MAX_VISIBLE - 2 && is_selected) {
        visible_start = state->cursor_pos - FIELD_MAX_VISIBLE + 2;
    }
    if (visible_start > len - FIELD_MAX_VISIBLE + 1) {
        visible_start = len - FIELD_MAX_VISIBLE + 1;
    }
    if (visible_start < 0) visible_start = 0;

    //draw text content
    if (is_selected) {
        con_set_color(COLOR_WHITE, 0);
    } else {
        con_set_color(COLOR_FG_DIM, 0);
    }

    //build visible string
    char visible[FIELD_MAX_VISIBLE + 1];
    int vis_len = 0;
    for (int i = 0; i < FIELD_MAX_VISIBLE && (visible_start + i) < len; i++) {
        visible[vis_len++] = text[visible_start + i];
    }
    visible[vis_len] = '\0';

    con_print_at(field_x, field_y, visible);

    //draw cursor if this field is selected
    if (is_selected) {
        int cursor_screen_pos = state->cursor_pos - visible_start;
        if (cursor_screen_pos >= 0 && cursor_screen_pos < FIELD_MAX_VISIBLE) {
            uint32_t cursor_x = field_x + cursor_screen_pos * char_w;
            uint32_t cursor_y = field_y;

            //draw white cursor block
            gfx_fill_rect(cursor_x, cursor_y, char_w, char_h, COLOR_WHITE);

            //draw character under cursor in black (if there is one)
            if (state->cursor_pos < len) {
                con_draw_char(cursor_x, cursor_y, text[state->cursor_pos], COLOR_BLACK, COLOR_WHITE);
            }
        }
    }
}

//draw the complete editor UI
static void editor_draw(EditorState *state) {
    Framebuffer *fb = gfx_get_fb();
    uint32_t char_h = con_get_char_height();
    uint32_t char_w = con_get_char_width();

    //clear screen
    gfx_clear(COLOR_BG);

    //title bar
    con_set_color(COLOR_WHITE, 0);
    char title[128];
    snprintf(title, sizeof(title), "DelBoot v0.6 - Edit: %s", state->name);
    con_print_at(char_w * 2, char_h, title);

    //separator
    con_set_color(COLOR_GRAY, 0);
    con_print_at(char_w * 2, char_h * 3, "----------------------------------------");

    //draw fields
    for (int i = 0; i < EDITOR_MAX_FIELDS; i++) {
        draw_field(state, i, i == state->field_index);
    }

    //help text at bottom
    uint32_t bottom_y = fb->height - char_h * 3;
    con_set_color(COLOR_GRAY, 0);
    con_print_at(char_w * 2, bottom_y, "F10: Boot    Escape: Cancel");
    con_print_at(char_w * 2, bottom_y + char_h, "Tab: Next field    Arrows: Navigate");
}

//initialize editor state from config entry
static int editor_init(EditorState *state, ConfigEntry *entry) {
    state->field_index = 0;
    state->cursor_pos = 0;

    //allocate name buffer
    gBS->AllocatePool(EfiLoaderData, 64, (void **)&state->name);
    if (!state->name) return -1;

    //allocate field buffers
    gBS->AllocatePool(EfiLoaderData, CONFIG_MAX_PATH, (void **)&state->fields[EDITOR_FIELD_PATH]);
    gBS->AllocatePool(EfiLoaderData, CONFIG_MAX_PATH, (void **)&state->fields[EDITOR_FIELD_INITRD]);
    gBS->AllocatePool(EfiLoaderData, CONFIG_MAX_CMDLINE, (void **)&state->fields[EDITOR_FIELD_CMDLINE]);

    if (!state->fields[EDITOR_FIELD_PATH] || !state->fields[EDITOR_FIELD_INITRD] || !state->fields[EDITOR_FIELD_CMDLINE]) {
        return -1;
    }

    //copy name (read-only)
    strncpy(state->name, entry->name, 63);
    state->name[63] = '\0';

    //copy editable fields
    strncpy(state->fields[EDITOR_FIELD_PATH], entry->path, CONFIG_MAX_PATH - 1);
    state->fields[EDITOR_FIELD_PATH][CONFIG_MAX_PATH - 1] = '\0';

    strncpy(state->fields[EDITOR_FIELD_INITRD], entry->initrd, CONFIG_MAX_PATH - 1);
    state->fields[EDITOR_FIELD_INITRD][CONFIG_MAX_PATH - 1] = '\0';

    strncpy(state->fields[EDITOR_FIELD_CMDLINE], entry->cmdline, CONFIG_MAX_CMDLINE - 1);
    state->fields[EDITOR_FIELD_CMDLINE][CONFIG_MAX_CMDLINE - 1] = '\0';

    return 0;
}

//free allocated editor state
static void editor_free(EditorState *state) {
    if (state->name) gBS->FreePool(state->name);
    if (state->fields[EDITOR_FIELD_PATH]) gBS->FreePool(state->fields[EDITOR_FIELD_PATH]);
    if (state->fields[EDITOR_FIELD_INITRD]) gBS->FreePool(state->fields[EDITOR_FIELD_INITRD]);
    if (state->fields[EDITOR_FIELD_CMDLINE]) gBS->FreePool(state->fields[EDITOR_FIELD_CMDLINE]);
}

//handle a keypress in the editor
static void editor_handle_key(EditorState *state, EFI_INPUT_KEY *key) {
    char *field = state->fields[state->field_index];
    int len = strlen(field);

    //check scan codes first
    if (key->ScanCode == SCAN_UP) {
        //move to previous field
        if (state->field_index > 0) {
            state->field_index--;
            //adjust cursor position for new field
            int new_len = strlen(state->fields[state->field_index]);
            if (state->cursor_pos > new_len) {
                state->cursor_pos = new_len;
            }
        }
        return;
    }

    if (key->ScanCode == SCAN_DOWN) {
        //move to next field
        if (state->field_index < EDITOR_MAX_FIELDS - 1) {
            state->field_index++;
            //adjust cursor position for new field
            int new_len = strlen(state->fields[state->field_index]);
            if (state->cursor_pos > new_len) {
                state->cursor_pos = new_len;
            }
        }
        return;
    }

    if (key->ScanCode == SCAN_LEFT) {
        //move cursor left
        if (state->cursor_pos > 0) {
            state->cursor_pos--;
        }
        return;
    }

    if (key->ScanCode == SCAN_RIGHT) {
        //move cursor right
        if (state->cursor_pos < len) {
            state->cursor_pos++;
        }
        return;
    }

    if (key->ScanCode == SCAN_HOME) {
        state->cursor_pos = 0;
        return;
    }

    if (key->ScanCode == SCAN_END) {
        state->cursor_pos = len;
        return;
    }

    if (key->ScanCode == SCAN_DELETE) {
        //delete character at cursor
        if (state->cursor_pos < len) {
            memmove(&field[state->cursor_pos],
                    &field[state->cursor_pos + 1],
                    len - state->cursor_pos);
        }
        return;
    }

    //check unicode characters
    if (key->UnicodeChar == 0x09) {
        //tab - move to next field (wrap around)
        state->field_index = (state->field_index + 1) % EDITOR_MAX_FIELDS;
        int new_len = strlen(state->fields[state->field_index]);
        if (state->cursor_pos > new_len) {
            state->cursor_pos = new_len;
        }
        return;
    }

    if (key->UnicodeChar == 0x08) {
        //backspace - delete character before cursor
        if (state->cursor_pos > 0) {
            memmove(&field[state->cursor_pos - 1],
                    &field[state->cursor_pos],
                    len - state->cursor_pos + 1);
            state->cursor_pos--;
        }
        return;
    }

    //printable character input
    if (key->UnicodeChar >= 0x20 && key->UnicodeChar <= 0x7E) {
        //insert character at cursor position
        int max_len = (state->field_index == EDITOR_FIELD_CMDLINE) ? CONFIG_MAX_CMDLINE : CONFIG_MAX_PATH;
        if (len < max_len - 1) {
            //shift existing characters right
            memmove(&field[state->cursor_pos + 1],
                    &field[state->cursor_pos],
                    len - state->cursor_pos + 1);
            field[state->cursor_pos] = (char)key->UnicodeChar;
            state->cursor_pos++;
        }
        return;
    }
}

int editor_run(ConfigEntry *entry) {
    EditorState state;

    //initialize state from config entry
    if (editor_init(&state, entry) != 0) {
        editor_free(&state);
        return EDITOR_CANCEL;
    }

    //initial draw
    editor_draw(&state);

    //input loop
    EFI_INPUT_KEY key;
    for (;;) {
        //check for key input
        EFI_STATUS status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);

        if (status == EFI_SUCCESS) {
            //check for boot (F10)
            if (key.ScanCode == SCAN_F10) {
                //copy edited values back to entry
                strncpy(entry->path, state.fields[EDITOR_FIELD_PATH], CONFIG_MAX_PATH - 1);
                entry->path[CONFIG_MAX_PATH - 1] = '\0';

                strncpy(entry->initrd, state.fields[EDITOR_FIELD_INITRD], CONFIG_MAX_PATH - 1);
                entry->initrd[CONFIG_MAX_PATH - 1] = '\0';

                strncpy(entry->cmdline, state.fields[EDITOR_FIELD_CMDLINE], CONFIG_MAX_CMDLINE - 1);
                entry->cmdline[CONFIG_MAX_CMDLINE - 1] = '\0';

                editor_free(&state);
                return EDITOR_BOOT;
            }

            //check for cancel (Escape)
            if (key.ScanCode == SCAN_ESCAPE || key.UnicodeChar == 0x1B) {
                editor_free(&state);
                return EDITOR_CANCEL;
            }

            //handle other keys
            editor_handle_key(&state, &key);

            //redraw after key handling
            editor_draw(&state);
        } else {
            //no key, wait a bit
            gBS->Stall(10000); //10ms
        }
    }
}
