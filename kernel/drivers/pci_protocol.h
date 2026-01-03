#ifndef DRIVERS_PCI_PROTOCOL_H
#define DRIVERS_PCI_PROTOCOL_H

#include <arch/types.h>
    
//message types
#define PCI_MSG_GET_INFO        1   //request device info
#define PCI_MSG_CONFIG_READ     2   //read config space
#define PCI_MSG_CONFIG_WRITE    3   //write config space
#define PCI_MSG_GET_BAR         4   //get BAR info

//response type (set in response messages)
#define PCI_MSG_RESPONSE        0x80000000

//message header (all messages start with this)
typedef struct {
    uint32 type;        //message type (request) or type | PCI_MSG_RESPONSE
    uint32 txn_id;      //transaction ID for matching request/response
    int32  status;      //0 = success, negative = error (in responses)
} pci_msg_hdr_t;

//GET_INFO response
typedef struct {
    pci_msg_hdr_t hdr;
    uint16 vendor_id;
    uint16 device_id;
    uint8  class_code;
    uint8  subclass;
    uint8  prog_if;
    uint8  header_type;
    uint8  int_line;
    uint8  int_pin;
    uint8  bus;
    uint8  dev;
    uint8  func;
    uint8  _pad;
} pci_msg_info_resp_t;

//CONFIG_READ request
typedef struct {
    pci_msg_hdr_t hdr;
    uint8  offset;
    uint8  size;    //1, 2, or 4 bytes
    uint8  _pad[2];
} pci_msg_config_read_req_t;

//CONFIG_READ response
typedef struct {
    pci_msg_hdr_t hdr;
    uint32 value;
} pci_msg_config_read_resp_t;

//CONFIG_WRITE request
typedef struct {
    pci_msg_hdr_t hdr;
    uint8  offset;
    uint8  size;    //1, 2, or 4 bytes
    uint8  _pad[2];
    uint32 value;
} pci_msg_config_write_req_t;

//CONFIG_WRITE response (just header with status)
typedef pci_msg_hdr_t pci_msg_config_write_resp_t;

//GET_BAR request
typedef struct {
    pci_msg_hdr_t hdr;
    uint8  bar_index;   //0-5
    uint8  _pad[3];
} pci_msg_get_bar_req_t;

//GET_BAR response
typedef struct {
    pci_msg_hdr_t hdr;
    uint64 addr;
    uint64 size;
    uint8  is_io;       //true if I/O space, false if MMIO
    uint8  is_64bit;    //true if 64-bit BAR
    uint8  _pad[6];
} pci_msg_get_bar_resp_t;

#endif
