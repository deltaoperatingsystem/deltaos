#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/rtc.h>
#include <drivers/pcspeaker.h>
#include <drivers/nvme.h>
#include <drivers/serial.h>
#include <drivers/vt/vt.h>
#include <obj/klog.h>
#include <obj/kernel_info.h>

#include "drivers_enabled.h"

void init_drivers(void) {
    //initialize drivers
#if DRIVER_FB
    fb_init();
    fb_init_backbuffer();
#endif
#if DRIVER_CONSOLE
    con_init();
#endif
#if DRIVER_VT
    vt_init();
#endif
#if DRIVER_KEYBOARD
    keyboard_init();
#endif
#if DRIVER_MOUSE
    mouse_init();
#endif
#if DRIVER_NVME
    nvme_init();
#endif
#if DRIVER_SERIAL
    serial_init_object();
#endif
#if DRIVER_RTC
    rtc_init();
#endif
#if DRIVER_PCSPEAKER
    pcspeaker_init();
#endif

    kernel_info_init();
    klog_init();
}