#include "menu.h"
#include "graphics.h"
#include "console.h"
#include "efi.h"
#include "stdio.h"

static MenuEntry entries[MENU_MAX_ENTRIES];
static int entry_count = 0;
static int selected = 0;

//set by main.c
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

void menu_init(void) {
    entry_count = 0;
    selected = 0;
}

int menu_add_entry(const char *name, const char *path) {
    if (entry_count >= MENU_MAX_ENTRIES) return -1;
    entries[entry_count].name = name;
    entries[entry_count].path = path;
    return entry_count++;
}

MenuEntry *menu_get_entry(int index) {
    if (index < 0 || index >= entry_count) return 0;
    return &entries[index];
}

static void draw_menu(void) {
    Framebuffer *fb = gfx_get_fb();
    uint32_t char_h = con_get_char_height();
    uint32_t char_w = con_get_char_width();
    
    //clear with black
    gfx_clear(COLOR_BG);
    
    //title
    con_set_color(COLOR_WHITE, 0);
    con_print_at(char_w * 2, char_h, "DelBoot v0.6");
    
    //seperator
    con_set_color(COLOR_GRAY, 0);
    con_print_at(char_w * 2, char_h * 3, "----------------------------------------");
    
    //entries
    uint32_t entry_y = char_h * 5;
    uint32_t entry_spacing = char_h + 4;
    
    for (int i = 0; i < entry_count; i++) {
        uint32_t y = entry_y + i * entry_spacing;
        
        if (i == selected) {
            //elected: white text with asterisk marker
            con_set_color(COLOR_HIGHLIGHT, 0);
            con_print_at(char_w * 2, y, "*");
            con_set_color(COLOR_WHITE, 0);
            con_print_at(char_w * 4, y, entries[i].name);
        } else {
            //unselected: gray text
            con_set_color(COLOR_FG_DIM, 0);
            con_print_at(char_w * 4, y, entries[i].name);
        }
    }
    
    uint32_t bottom_y = fb->height - char_h * 4;
    con_set_color(COLOR_GRAY, 0);
    con_print_at(char_w * 2, bottom_y, "Use the arrow keys to select which entry is highlighted.");
    con_print_at(char_w * 2, bottom_y + char_h, "Press enter to boot the selected entry");
    con_print_at(char_w * 2, bottom_y + char_h * 2, "Press 'g' to change graphics resolution");
}

int menu_run(int timeout, int default_index) {
    if (entry_count == 0) return -1;
    
    selected = default_index;
    if (selected < 0 || selected >= entry_count) selected = 0;

    draw_menu();
    
    //wait for key input
    EFI_INPUT_KEY key;
    
    uint64_t timeout_ms = (uint64_t)timeout * 1000;
    uint64_t elapsed_ms = 0;
    int timeout_active = (timeout > 0);

    for (;;) {
        //handle timeout display
        if (timeout_active) {
            uint32_t char_h = con_get_char_height();
            uint32_t char_w = con_get_char_width();
            Framebuffer *fb = gfx_get_fb();
            uint32_t y = fb->height - char_h * 5;
            
            char msg[64];
            uint32_t remaining = (timeout_ms - elapsed_ms + 999) / 1000;
            
            snprintf(msg, sizeof(msg), "Booting automatically in %u seconds...  ", remaining);
            
            con_set_color(COLOR_WHITE, COLOR_BG);
            con_print_at(char_w * 2, y, msg);
            
            if (elapsed_ms >= timeout_ms) {
                return selected;
            }
        }

        //check for key without blocking
        EFI_STATUS status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        
        if (status == EFI_SUCCESS) {
            //key pressed so cancel timeout if active
            if (timeout_active) {
                timeout_active = 0;
                //clear the countdown line
                uint32_t char_h = con_get_char_height();
                uint32_t char_w = con_get_char_width();
                Framebuffer *fb = gfx_get_fb();
                uint32_t y = fb->height - char_h * 5;
                con_print_at(char_w * 2, y, "                                        ");
            }

            //handle key
            if (key.ScanCode == 0x01) {  //up arrows
                if (selected > 0) {
                    selected--;
                    draw_menu();
                }
            } else if (key.ScanCode == 0x02) {  //down arrow
                if (selected < entry_count - 1) {
                    selected++;
                    draw_menu();
                }
            } else if (key.UnicodeChar == 0x0D || key.UnicodeChar == '\r') {  //enter
                return selected;
            } else if (key.UnicodeChar == 'g' || key.UnicodeChar == 'G') {
                return MENU_GOP_REQUEST;
            }
        } else {
            //no key, wait a bit
            gBS->Stall(10000); //10ms
            if (timeout_active) {
                elapsed_ms += 10;
            }
        }
    }
}
