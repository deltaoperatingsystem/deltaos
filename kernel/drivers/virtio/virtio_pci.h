#ifndef DRIVERS_VIRTIO_PCI_H
#define DRIVERS_VIRTIO_PCI_H

#include "virtio.h"
#include <drivers/pci.h>

//virtio PCI capability types (virtio spec 4.1.4)
#define VIRTIO_PCI_CAP_COMMON_CFG 1 //common configuration
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2 //notification
#define VIRTIO_PCI_CAP_ISR_CFG 3 //ISR status
#define VIRTIO_PCI_CAP_DEVICE_CFG 4  //device-specific configuration
#define VIRTIO_PCI_CAP_PCI_CFG 5 //alternative PCI config access

//virtio PCI capability structure (sits in PCI config space capability list)
typedef struct {
    uint8 cap_vndr; //PCI cap ID (always 0x09 for vendor-specific)
    uint8 cap_next; //next capability offset
    uint8 cap_len;  //length of this capability
    uint8 cfg_type; //VIRTIO_PCI_CAP_* above
    uint8 bar; //which BAR the region lives in
    uint8 padding[3];
    uint32 offset; //byte offset within the BAR
    uint32 length; //length of the region
} __attribute__((packed)) virtio_pci_cap_t;

//common configuration structure (mapped via VIRTIO_PCI_CAP_COMMON_CFG)
typedef struct {
    uint32 device_feature_select;
    uint32 device_feature;
    uint32 driver_feature_select;
    uint32 driver_feature;
    uint16 msix_config;
    uint16 num_queues;
    uint8 device_status;
    uint8 config_generation;
    uint16 queue_select;
    uint16 queue_size;
    uint16 queue_msix_vector;
    uint16 queue_enable;
    uint16 queue_notify_off;
    uint64 queue_desc;
    uint64 queue_driver;
    uint64 queue_device;
} __attribute__((packed)) virtio_pci_common_cfg_t;

//per-device PCI transport state
typedef struct {
    pci_device_t *pci;
    volatile virtio_pci_common_cfg_t *common; //mapped common cfg region
    void *notify_base;//mapped notify region
    uint32 notify_off_multiplier;
    void *isr; //mapped ISR region
    void *device_cfg; //mapped device-specific cfg region

    //MSI-X for the controlq (queue 0)
    uint8 msix_cap_ptr;
    void *msix_table; //mapped MSI-X table
    uint8 msix_vector; //vector we allocated for the controlq
    bool msix_ok; //true if MSI-X was set up successfully
} virtio_pci_data_t;

//scan all PCI devices and register any virtio devices found
void virtio_pci_init(void);

#endif
