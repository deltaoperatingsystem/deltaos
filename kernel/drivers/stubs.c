#include <arch/types.h>
#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/pcspeaker.h>
#include <drivers/pci.h>
#include <drivers/nvme.h>
#include <drivers/rtc.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/vt/vt.h>

#define WEAK __attribute__((weak))

//framebuffer stubs
WEAK void fb_init(void) {}
WEAK void fb_init_backbuffer(void) {}
WEAK void fb_flip(void) {}
WEAK bool fb_available(void) { return false; }
WEAK uint32 fb_width(void) { return 0; }
WEAK uint32 fb_height(void) { return 0; }
WEAK void fb_clear(uint32 color) { (void)color; }
WEAK void fb_putpixel(uint32 x, uint32 y, uint32 color) { (void)x; (void)y; (void)color; }
WEAK void fb_fillrect(uint32 x, uint32 y, uint32 w, uint32 h, uint32 color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
WEAK void fb_drawline(uint32 x1, uint32 y1, uint32 x2, uint32 y2, uint32 colour) { (void)x1; (void)y1; (void)x2; (void)y2; (void)colour; }
WEAK void fb_drawimage(const unsigned char *src, uint32 width, uint32 height, uint32 x, uint32 y) { (void)src; (void)width; (void)height; (void)x; (void)y; }
WEAK void fb_scroll(uint32 lines, uint32 bg_color) { (void)lines; (void)bg_color; }

//console stubs
WEAK void con_init(void) {}
WEAK void con_clear(void) {}
WEAK void con_flush(void) {}
WEAK void con_putc(char c) { (void)c; }
WEAK void con_print(const char *s) { (void)s; }
WEAK uint32 con_cols(void) { return 0; }
WEAK uint32 con_rows(void) { return 0; }
WEAK void con_draw_char_at(uint32 col, uint32 row, char c, uint32 fg, uint32 bg) { (void)col; (void)row; (void)c; (void)fg; (void)bg; }

//PC speaker stubs
WEAK void pcspeaker_init(void) {}
WEAK void pcspeaker_beep(uint32 freq) { (void)freq; }
WEAK void pcspeaker_stop(void) {}

//PCI stubs
WEAK void pci_init(void) {}
WEAK pci_device_t *pci_find_device(uint16 vendor_id, uint16 device_id) { (void)vendor_id; (void)device_id; return NULL; }
WEAK pci_device_t *pci_find_class(uint8 class_code, uint8 subclass) { (void)class_code; (void)subclass; return NULL; }
WEAK pci_device_t *pci_get_device(uint8 bus, uint8 dev, uint8 func) { (void)bus; (void)dev; (void)func; return NULL; }
WEAK pci_device_t *pci_get_devices(void) { return NULL; }
WEAK uint32 pci_device_count(void) { return 0; }
WEAK uint32 pci_config_read(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size) { (void)bus; (void)dev; (void)func; (void)offset; (void)size; return 0; }
WEAK void pci_config_write(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size, uint32 value) { (void)bus; (void)dev; (void)func; (void)offset; (void)size; (void)value; }
WEAK void pci_enable_bus_master(pci_device_t *dev) { (void)dev; }
WEAK void pci_enable_mmio(pci_device_t *dev) { (void)dev; }
WEAK void pci_enable_io(pci_device_t *dev) { (void)dev; }

//NVmE stubs
WEAK void nvme_init(void) {}
WEAK void nvme_msix_handler(nvme_ctrl_t *ctrl, uint16 qid) { (void)ctrl; (void)qid; }
WEAK void nvme_isr_callback(uint64 vector) { (void)vector; }

//serial stubs
WEAK void serial_init(void) {}
WEAK void serial_init_object(void) {}
WEAK void serial_write_char(char c) { (void)c; }
WEAK void serial_write(const char *s) { (void)s; }
WEAK void serial_write_hex(uint64 n) { (void)n; }

//RTC stubs
WEAK void rtc_init(void) {}
WEAK void rtc_get_time(rtc_time_t *time) { if (time) { time->second = time->minute = time->hour = time->day = time->month = 0; time->year = 1970; } }

//keyboard stubs
WEAK void keyboard_init(void) {}
WEAK void keyboard_irq(void) {}
WEAK bool get_key(char *c) { (void)c; return false; }
WEAK bool get_keystate(char c) { (void)c; return false; }
WEAK void keyboard_wait(void) {}

//mouse stubs
WEAK void mouse_init(void) {}
WEAK void mouse_irq(void) {}

//VT stubs
WEAK void vt_init(void) {}
WEAK vt_t *vt_create(void) { return NULL; }
WEAK void vt_destroy(vt_t *vt) { (void)vt; }
WEAK vt_t *vt_get(int num) { (void)num; return NULL; }
WEAK vt_t *vt_get_active(void) { return NULL; }
WEAK void vt_switch(int num) { (void)num; }
WEAK bool vt_poll_event(vt_t *vt, vt_event_t *event) { (void)vt; (void)event; return false; }
WEAK void vt_wait_event(vt_t *vt, vt_event_t *event) { (void)vt; (void)event; }
WEAK void vt_push_event(vt_t *vt, const vt_event_t *event) { (void)vt; (void)event; }
WEAK void vt_set_attr(vt_t *vt, int attr, uint32 value) { (void)vt; (void)attr; (void)value; }
WEAK void vt_write(vt_t *vt, const char *s, size len) { (void)vt; (void)s; (void)len; }
WEAK void vt_putc(vt_t *vt, char c) { (void)vt; (void)c; }
WEAK void vt_print(vt_t *vt, const char *s) { (void)vt; (void)s; }
WEAK void vt_clear(vt_t *vt) { (void)vt; }
WEAK void vt_set_cursor(vt_t *vt, uint32 col, uint32 row) { (void)vt; (void)col; (void)row; }
WEAK uint32 vt_cols(vt_t *vt) { (void)vt; return 0; }
WEAK uint32 vt_rows(vt_t *vt) { (void)vt; return 0; }
WEAK void vt_flush(vt_t *vt) { (void)vt; }
WEAK void vt_write_cells(vt_t *vt, uint32 col, uint32 row, const vt_cell_t *cells, size count) { (void)vt; (void)col; (void)row; (void)cells; (void)count; }
