#include <types.h>

#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 19

typedef struct {
    uint16 mask;   //1=pixel visible
    uint16 color;  //1=white, 0=black (only matters where mask=1)
} cursor_row_t;

static const cursor_row_t cursor_data[CURSOR_HEIGHT] = {
    { 0x800, 0x000 },  //X
    { 0xC00, 0x400 },  //XW
    { 0xE00, 0x600 },  //XWW
    { 0xF00, 0x700 },  //XWWW
    { 0xF80, 0x780 },  //XWWWW
    { 0xFC0, 0x7C0 },  //XWWWWW
    { 0xFE0, 0x7E0 },  //XWWWWWW
    { 0xFF0, 0x7F0 },  //XWWWWWWW
    { 0xFF8, 0x7F8 },  //XWWWWWWWW
    { 0xFFC, 0x7FC },  //XWWWWWWWWW
    { 0xFFE, 0x7FE },  //XWWWWWWWWWW
    { 0xFFF, 0x7C0 },  //XWWWWWXXXXXX
    { 0xEE0, 0x6E0 },  //XWWXWWW
    { 0xE70, 0x470 },  //XWX.XWW
    { 0xC70, 0x070 },  //XX..XWW
    { 0x838, 0x038 },  //X....XWW
    { 0x038, 0x038 },  //.....XWW
    { 0x01C, 0x01C },  //......XWW
    { 0x018, 0x000 },  //......XX
};

//get cursor dimensions
int cursor_get_width(void) { return CURSOR_WIDTH; }
int cursor_get_height(void) { return CURSOR_HEIGHT; }

//get pixel at (x, y) - returns 0xAARRGGBB or 0 for transparent
uint32 cursor_get_pixel(int x, int y) {
    if (x < 0 || x >= CURSOR_WIDTH || y < 0 || y >= CURSOR_HEIGHT) return 0;
    
    uint16 bit = 1 << (CURSOR_WIDTH - 1 - x);
    
    if (!(cursor_data[y].mask & bit)) return 0;  //transparent
    
    if (cursor_data[y].color & bit) {
        return 0xFFFFFFFF;  //white
    } else {
        return 0xFF000000;  //black
    }
}
