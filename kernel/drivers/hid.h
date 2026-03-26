#ifndef DRIVERS_HID_H
#define DRIVERS_HID_H

#include <arch/types.h>

//HID protocol values (matches USB_HID_PROTO_*)
#define HID_PROTO_KEYBOARD  1
#define HID_PROTO_MOUSE     2

//called by xhci_process_events() when a HID interrupt transfer completes
//proto  : HID_PROTO_KEYBOARD or HID_PROTO_MOUSE
//report : pointer to the raw report bytes (virtual, DMA buffer)
//len    : number of bytes actually received
void hid_report_received(uint8 proto, const void *report, uint32 len);

//parse a HID report descriptor and cache the resulting report profile
//returns true on success and writes the resolved protocol to proto_out
bool hid_parse_report_descriptor(const void *desc, uint32 len,
                                 uint8 *proto_out,
                                 uint8 *report_id_out,
                                 uint16 *report_len_out);

//true once USB HID traffic has been observed for the given class
//used to suppress legacy PS/2 paths when USB HID is active
bool hid_usb_keyboard_active(void);
bool hid_usb_mouse_active(void);
void hid_usb_keyboard_detached(void);
void hid_usb_mouse_detached(void);

#endif
