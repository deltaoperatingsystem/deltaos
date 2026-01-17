#include <types.h>

#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 19

typedef struct {
    uint16 mask;   //1=pixel visible
    uint16 color;  //1=white, 0=black (only matters where mask=1)
} cursor_row_t;

static const cursor_row_t cursor_data[CURSOR_HEIGHT] = {
    { 0b100000000000, 0b000000000000 },  //X
    { 0b110000000000, 0b010000000000 },  //XW
    { 0b111000000000, 0b011000000000 },  //XWW
    { 0b111100000000, 0b011100000000 },  //XWWW
    { 0b111110000000, 0b011110000000 },  //XWWWW
    { 0b111111000000, 0b011111000000 },  //XWWWWW
    { 0b111111100000, 0b011111100000 },  //XWWWWWW
    { 0b111111110000, 0b011111110000 },  //XWWWWWWW
    { 0b111111111000, 0b011111111000 },  //XWWWWWWWW
    { 0b111111111100, 0b011111111100 },  //XWWWWWWWWW
    { 0b111111111110, 0b011111111110 },  //XWWWWWWWWWW
    { 0b111111111111, 0b011111000000 },  //XWWWWWXXXXXX
    { 0b111011100000, 0b011011100000 },  //XWWXWWW
    { 0b111001110000, 0b010001110000 },  //XWX.XWW
    { 0b110001110000, 0b000001110000 },  //XX..XWW
    { 0b100000111000, 0b000000111000 },  //X....XWW
    { 0b000000111000, 0b000000111000 },  //.....XWW
    { 0b000000011100, 0b000000011100 },  //......XWW
    { 0b000000011000, 0b000000000000 },  //......XX
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
