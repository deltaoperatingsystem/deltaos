#ifndef DRIVERS_VIRTIO_MMIO_H
#define DRIVERS_VIRTIO_MMIO_H

#include "virtio.h"

//virtio-mmio register offsets (version 2, modern QEMU)
#define VIRTIO_MMIO_MAGIC_VALUE         0x000 //must read as 0x74726976 ("virt")
#define VIRTIO_MMIO_VERSION             0x004 //must be 2 for modern
#define VIRTIO_MMIO_DEVICE_ID           0x008 //device type (VIRTIO_DEV_*)
#define VIRTIO_MMIO_VENDOR_ID           0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW    0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH   0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW    0x0A0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH   0x0A4
#define VIRTIO_MMIO_CONFIG              0x100

#define VIRTIO_MMIO_MAGIC  0x74726976

//per-device MMIO transport state
typedef struct {
    uintptr base;       //mapped MMIO base (virtual)
} virtio_mmio_data_t;

//probe a known MMIO base address and register the device if valid
//used by platform code that knows where virtio-mmio devices live
bool virtio_mmio_init_at(uintptr phys_base);

#endif
