#ifndef DRIVERS_USB_H
#define DRIVERS_USB_H

#include <arch/types.h>

//USB descriptor types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

//USB device classes
#define USB_CLASS_HID           0x03

//HID subclasses
#define USB_HID_SUBCLASS_NONE   0x00
#define USB_HID_SUBCLASS_BOOT   0x01

//HID protocols
#define USB_HID_PROTO_NONE      0x00
#define USB_HID_PROTO_KEYBOARD  0x01
#define USB_HID_PROTO_MOUSE     0x02

//standard USB requests (bRequest)
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09

//HID class requests
#define USB_HID_REQ_SET_IDLE    0x0A
#define USB_HID_REQ_SET_PROTO   0x0B

//bmRequestType direction bit
#define USB_DIR_H2D             0x00    //host to device
#define USB_DIR_D2H             0x80    //device to host

//bmRequestType type field
#define USB_TYPE_STD            0x00    //standard
#define USB_TYPE_CLASS          0x20    //class
#define USB_TYPE_VENDOR         0x40    //vendor

//bmRequestType recipient field
#define USB_RECIP_DEVICE        0x00
#define USB_RECIP_IFACE         0x01
#define USB_RECIP_EP            0x02

//endpoint transfer types (bmAttributes bits [1:0])
#define USB_EP_XFER_CTRL        0x00
#define USB_EP_XFER_ISOCH       0x01
#define USB_EP_XFER_BULK        0x02
#define USB_EP_XFER_INTR        0x03

//endpoint direction (bEndpointAddress bit 7)
#define USB_EP_DIR_OUT          0x00
#define USB_EP_DIR_IN           0x80
#define USB_EP_ADDR(addr)       ((addr) & 0x0F)
#define USB_EP_IS_IN(addr)      (!!((addr) & 0x80))

//xHCI port speed encoding (PortSC PSI field bits [13:10])
#define USB_SPEED_FS            1   //full speed  12 Mbit/s
#define USB_SPEED_LS            2   //low speed   1.5 Mbit/s
#define USB_SPEED_HS            3   //high speed  480 Mbit/s
#define USB_SPEED_SS            4   //SuperSpeed  5 Gbit/s
#define USB_SPEED_SSP           5   //SuperSpeed+ 10 Gbit/s

//default EP0 max packet sizes per speed (refined after GET_DESCRIPTOR)
#define USB_MPS0_LS             8
#define USB_MPS0_FS             8
#define USB_MPS0_HS             64
#define USB_MPS0_SS             512

static inline uint16 usb_default_mps0(uint8 speed) {
    switch (speed) {
        case USB_SPEED_LS:  return USB_MPS0_LS;
        case USB_SPEED_FS:  return USB_MPS0_FS;
        case USB_SPEED_HS:  return USB_MPS0_HS;
        case USB_SPEED_SS:
        case USB_SPEED_SSP: return USB_MPS0_SS;
        default:            return 8;
    }
}


//descriptor structs (all packed as they arrive from the device)
typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;     //USB_DESC_DEVICE
    uint16 bcdUSB;
    uint8  bDeviceClass;
    uint8  bDeviceSubClass;
    uint8  bDeviceProtocol;
    uint8  bMaxPacketSize0;     //EP0 max packet size
    uint16 idVendor;
    uint16 idProduct;
    uint16 bcdDevice;
    uint8  iManufacturer;
    uint8  iProduct;
    uint8  iSerialNumber;
    uint8  bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;     //USB_DESC_CONFIG
    uint16 wTotalLength;        //total length of config + all subordinate descs
    uint8  bNumInterfaces;
    uint8  bConfigurationValue;
    uint8  iConfiguration;
    uint8  bmAttributes;
    uint8  bMaxPower;           //in units of 2 mA
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;     //USB_DESC_INTERFACE
    uint8  bInterfaceNumber;
    uint8  bAlternateSetting;
    uint8  bNumEndpoints;       //excludes EP0
    uint8  bInterfaceClass;
    uint8  bInterfaceSubClass;
    uint8  bInterfaceProtocol;
    uint8  iInterface;
} usb_iface_desc_t;

typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;     //USB_DESC_ENDPOINT
    uint8  bEndpointAddress;    //bit 7 = dir (1=IN), bits [3:0] = EP number
    uint8  bmAttributes;        //bits [1:0] = transfer type
    uint16 wMaxPacketSize;
    uint8  bInterval;           //polling interval in (ms for FS/LS, 125µs for HS)
} usb_ep_desc_t;

typedef struct __attribute__((packed)) {
    uint8  bLength;
    uint8  bDescriptorType;     //USB_DESC_HID
    uint16 bcdHID;
    uint8  bCountryCode;
    uint8  bNumDescriptors;
    uint8  bDescriptorType2;    //USB_DESC_HID_REPORT
    uint16 wDescriptorLength;
} usb_hid_desc_t;

//8-byte USB setup packet (sent in Setup Stage TRB)
typedef struct __attribute__((packed)) {
    uint8  bmRequestType;
    uint8  bRequest;
    uint16 wValue;
    uint16 wIndex;
    uint16 wLength;
} usb_setup_pkt_t;

#endif
