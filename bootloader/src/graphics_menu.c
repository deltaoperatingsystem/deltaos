#include "graphics_menu.h"
#include "graphics.h"
#include "console.h"
#include "stdio.h"
#include <string.h>

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;

static void draw_gfx_menu(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, uint32_t selected_mode, uint32_t start_index, uint32_t max_display) {
    uint32_t char_h = con_get_char_height();
    uint32_t char_w = con_get_char_width();
    
    gfx_clear(COLOR_BG);
    
    con_set_color(COLOR_WHITE, 0);
    con_print_at(char_w * 2, char_h, "Graphics Mode Selection");
    
    con_set_color(COLOR_GRAY, 0);
    con_print_at(char_w * 2, char_h * 3, "----------------------------------------");
    
    uint32_t entry_y = char_h * 5;
    uint32_t entry_spacing = char_h + 2;
    
    uint32_t display_count = 0;
    for (uint32_t i = start_index; i < gop->Mode->MaxMode && display_count < max_display; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN size;
        EFI_STATUS status = gop->QueryMode(gop, i, &size, &info);
        
        if (EFI_ERROR(status)) continue;
        
        uint32_t y = entry_y + display_count * entry_spacing;
        
        char buf[128];
        snprintf(buf, sizeof(buf), "%u: %ux%u (Pitch: %u)", i, info->HorizontalResolution, info->VerticalResolution, info->PixelsPerScanLine);
        
        if (i == selected_mode) {
            con_set_color(COLOR_HIGHLIGHT, 0);
            con_print_at(char_w * 2, y, ">");
            con_set_color(COLOR_WHITE, 0);
            con_print_at(char_w * 4, y, buf);
        } else {
            con_set_color(COLOR_FG_DIM, 0);
            con_print_at(char_w * 4, y, buf);
        }
        
        gBS->FreePool(info);
        display_count++;
    }
    
    Framebuffer *fb = gfx_get_fb();
    con_set_color(COLOR_GRAY, 0);
    con_print_at(char_w * 2, fb->height - char_h * 4, "Use Arrow Keys to select, Enter to confirm, Esc to cancel.");
    char mode_buf[64];
    snprintf(mode_buf, sizeof(mode_buf), "Current Mode: %u", gop->Mode->Mode);
    con_print_at(char_w * 2, fb->height - char_h * 2, mode_buf);
}

void gfx_menu_run(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    uint32_t selected = gop->Mode->Mode;
    uint32_t start_index = 0;
    uint32_t max_display = 20;
    
    draw_gfx_menu(gop, selected, start_index, max_display);
    
    EFI_INPUT_KEY key;
    for (;;) {
        EFI_STATUS status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (status == EFI_SUCCESS) {
            if (key.ScanCode == 0x01) { //up
                if (selected > 0) {
                    selected--;
                    if (selected < start_index) start_index = selected;
                    draw_gfx_menu(gop, selected, start_index, max_display);
                }
            } else if (key.ScanCode == 0x02) { //down
                if (selected < gop->Mode->MaxMode - 1) {
                    selected++;
                    if (selected >= start_index + max_display) start_index = selected - max_display + 1;
                    draw_gfx_menu(gop, selected, start_index, max_display);
                }
            } else if (key.UnicodeChar == 0x0D || key.UnicodeChar == '\r') { //enter
                status = gop->SetMode(gop, selected);
                if (!EFI_ERROR(status)) {
                    //re-init graphics state
                    gfx_init(
                        gop->Mode->FrameBufferBase,
                        gop->Mode->Info->HorizontalResolution,
                        gop->Mode->Info->VerticalResolution,
                        gop->Mode->Info->PixelsPerScanLine
                    );
                    con_init();
                }
                return;
            } else if (key.ScanCode == 0x17 || key.UnicodeChar == 0x1B) { // Esc
                return;
            }
        }
        gBS->Stall(10000);
    }
}
