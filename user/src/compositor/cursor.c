#include <types.h>

#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 19

typedef struct {
    uint16 mask;
    uint16 color;
} cursor_row_t;

//mask selects which pixels to draw, color=black for outline, white for fill
static const cursor_row_t cursor_data[CURSOR_HEIGHT] = {
    { 0x800, 0x000 },
    { 0xC00, 0x400 },
    { 0xE00, 0x600 },
    { 0xF00, 0x700 },
    { 0xF80, 0x780 },
    { 0xFC0, 0x7C0 },
    { 0xFE0, 0x7E0 },
    { 0xFF0, 0x7F0 },
    { 0xFF8, 0x7F8 },
    { 0xFFC, 0x7FC },
    { 0xFFE, 0x7FE },
    { 0xFFF, 0x7C0 },
    { 0xEE0, 0x6E0 },
    { 0xE70, 0x470 },
    { 0xC70, 0x070 },
    { 0x838, 0x038 },
    { 0x038, 0x038 },
    { 0x01C, 0x01C },
    { 0x018, 0x000 },
};

int cursor_get_width(void) { return CURSOR_WIDTH; }
int cursor_get_height(void) { return CURSOR_HEIGHT; }

//returns 0=transparent, 0xFFFFFFFF=white, 0xFF000000=black
uint32 cursor_get_pixel(int x, int y) {
    if (x < 0 || x >= CURSOR_WIDTH || y < 0 || y >= CURSOR_HEIGHT) return 0;
    uint16 bit = 1 << (CURSOR_WIDTH - 1 - x);
    if (!(cursor_data[y].mask & bit)) return 0;
    if (cursor_data[y].color & bit)
        return 0xFFFFFFFF;
    else
        return 0xFF000000;
}
