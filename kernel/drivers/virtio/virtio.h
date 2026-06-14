#ifndef DRIVERS_VIRTIO_H
#define DRIVERS_VIRTIO_H

#include <arch/types.h>
#include <lib/spinlock.h>
#include <proc/wait.h>

//virtio device type IDs
#define VIRTIO_DEV_GPU 16  //GPU 2D/3D (type 16, PCI device ID 0x1050)

//virtio vendor ID (all virtio PCI devices use this)
#define VIRTIO_PCI_VENDOR 0x1AF4

//virtio feature bits
#define VIRTIO_F_VERSION_1 (1ULL << 32)

//device status bits
#define VIRTIO_STATUS_ACKNOWLEDGE (1 << 0)
#define VIRTIO_STATUS_DRIVER (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK (1 << 3)
#define VIRTIO_STATUS_FAILED (1 << 7)

//max queue size we'll ever use
#define VIRTQ_MAX_SIZE 256

//split-virtqueue structures (virtio spec section 2.6)
typedef struct {
    uint64 addr; //physical address of buffer
    uint32 len; //length of buffer
    uint16 flags; //VIRTQ_DESC_F_* below
    uint16 next; //next descriptor index (if F_NEXT is set)
} __attribute__((packed)) virtq_desc_t;

#define VIRTQ_DESC_F_NEXT (1 << 0)  //descriptor chains to the next
#define VIRTQ_DESC_F_WRITE (1 << 1)  //device writes into this buffer
#define VIRTQ_DESC_F_INDIRECT (1 << 2)  //buffer is an indirect descriptor table

//available ring (driver -> device)
typedef struct {
    uint16 flags; //0 = normal, 1 = suppress used ring interrupts
    uint16 idx; //next slot driver will write
    uint16 ring[]; //descriptor head indices
} __attribute__((packed)) virtq_avail_t;

//used ring element
typedef struct {
    uint32 id; //descriptor head index
    uint32 len; //total bytes written by device
} __attribute__((packed)) virtq_used_elem_t;

//used ring (device -> driver)
typedef struct {
    uint16 flags;
    uint16 idx; //next slot device will write
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

//high-level queue object
typedef struct {
    virtq_desc_t  *desc;
    uint16 desc_count; //power-of-2 queue size
    uint16 free_head; //head of the free descriptor list
    uint16 free_count;

    virtq_avail_t *avail;

    virtq_used_t *used;
    uint16 last_used_idx;   //driver-side cursor into the used ring

    //waiter wakes up when last_used_idx != used->idx (async flush)
    wait_queue_t wq;
    spinlock_t lock;

    uint16 queue_idx; //notify index, used by transport notify()
} virtq_t;

//abstract virtio device
struct virtio_device;

typedef struct virtio_transport {
    //device-specific config space access
    uint8 (*read_cfg8) (struct virtio_device *dev, uint32 offset);
    uint16 (*read_cfg16)(struct virtio_device *dev, uint32 offset);
    uint32 (*read_cfg32)(struct virtio_device *dev, uint32 offset);
    void (*write_cfg8) (struct virtio_device *dev, uint32 offset, uint8  val);
    void (*write_cfg16)(struct virtio_device *dev, uint32 offset, uint16 val);
    void (*write_cfg32)(struct virtio_device *dev, uint32 offset, uint32 val);

    uint8 (*read_status) (struct virtio_device *dev);
    void (*write_status)(struct virtio_device *dev, uint8 status);

    uint64 (*read_features) (struct virtio_device *dev);
    void (*write_features)(struct virtio_device *dev, uint64 features);

    //set up a virtqueue and fill in vq->desc/avail/used
    int (*setup_queue)(struct virtio_device *dev, uint16 queue_idx,
                       uint16 queue_size, virtq_t *vq);

    //ring the doorbell for a queue
    void (*notify)(struct virtio_device *dev, virtq_t *vq);
} virtio_transport_t;

typedef struct virtio_device {
    uint32 device_type; //VIRTIO_DEV_* (from subsystem device ID)
    const virtio_transport_t *transport;
    void  *transport_data; //private to the transport implementation

    struct virtio_device *next; //global device list
} virtio_device_t;

//global list of all discovered virtio devices
extern virtio_device_t *virtio_devices;

//virtqueue helpers

//init the free-descriptor chain for a freshly allocated queue
void virtq_init(virtq_t *vq, uint16 size);

//allocate one descriptor from the free list, returns index or -1 if full
int  virtq_alloc_desc(virtq_t *vq);

//return a descriptor back to the free list
void virtq_free_desc(virtq_t *vq, uint16 idx);

//push a chain of descriptors into the available ring and ring the doorbell
void virtq_kick(virtio_device_t *dev, virtq_t *vq, uint16 head);

//poll the used ring until the entry for head_idx appears, returns bytes written by device
int  virtq_poll_used(virtq_t *vq, uint16 head_idx);

//run the standard virtio device init sequence (spec 3.1.1)
//driver_features is the feature set the driver wants to negotiate
int virtio_dev_init(virtio_device_t *dev, uint64 driver_features);

#endif
