#ifndef _FB_H
#define _FB_H

#include <types.h>

#define FB_RGB(r, g, b) ((uint32)((b) | ((g) << 8) | ((r) << 16)))

#define FB_BLACK   FB_RGB(0, 0, 0)
#define FB_WHITE   FB_RGB(255, 255, 255)
#define FB_RED     FB_RGB(255, 0, 0)
#define FB_GREEN   FB_RGB(0, 255, 0)
#define FB_BLUE    FB_RGB(0, 0, 255)
#define FB_GRAY    FB_RGB(128, 128, 128)

#endif