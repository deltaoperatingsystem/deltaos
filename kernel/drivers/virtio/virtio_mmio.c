#include "virtio_mmio.h"
#include "virtio.h"
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <arch/mmu.h>
#include <lib/io.h>
#include <lib/string.h>

static inline uint32 mmio_read32(uintptr base, uint32 reg) {
    return *(volatile uint32 *)(base + reg);
}

static inline void mmio_write32(uintptr base, uint32 reg, uint32 val) {
    *(volatile uint32 *)(base + reg) = val;
}

//transport ops
static uint8 mmio_read_cfg8(struct virtio_device *dev, uint32 offset) {
    virtio_mmio_data_t *d = dev->transport_data;
    return *(volatile uint8 *)(d->base + VIRTIO_MMIO_CONFIG + offset);
}

static uint16 mmio_read_cfg16(struct virtio_device *dev, uint32 offset) {
    virtio_mmio_data_t *d = dev->transport_data;
    return *(volatile uint16 *)(d->base + VIRTIO_MMIO_CONFIG + offset);
}

static uint32 mmio_read_cfg32(struct virtio_device *dev, uint32 offset) {
    virtio_mmio_data_t *d = dev->transport_data;
    return mmio_read32(d->base, VIRTIO_MMIO_CONFIG + offset);
}

static void mmio_write_cfg8(struct virtio_device *dev, uint32 offset, uint8 val) {
    virtio_mmio_data_t *d = dev->transport_data;
    *(volatile uint8 *)(d->base + VIRTIO_MMIO_CONFIG + offset) = val;
}

static void mmio_write_cfg16(struct virtio_device *dev, uint32 offset, uint16 val) {
    virtio_mmio_data_t *d = dev->transport_data;
    *(volatile uint16 *)(d->base + VIRTIO_MMIO_CONFIG + offset) = val;
}

static void mmio_write_cfg32(struct virtio_device *dev, uint32 offset, uint32 val) {
    virtio_mmio_data_t *d = dev->transport_data;
    mmio_write32(d->base, VIRTIO_MMIO_CONFIG + offset, val);
}

static uint8 mmio_read_status(struct virtio_device *dev) {
    virtio_mmio_data_t *d = dev->transport_data;
    return (uint8)mmio_read32(d->base, VIRTIO_MMIO_STATUS);
}

static void mmio_write_status(struct virtio_device *dev, uint8 status) {
    virtio_mmio_data_t *d = dev->transport_data;
    mmio_write32(d->base, VIRTIO_MMIO_STATUS, status);
}

static uint64 mmio_read_features(struct virtio_device *dev) {
    virtio_mmio_data_t *d = dev->transport_data;
    mmio_write32(d->base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint64 lo = mmio_read32(d->base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_write32(d->base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint64 hi = mmio_read32(d->base, VIRTIO_MMIO_DEVICE_FEATURES);
    return lo | (hi << 32);
}

static void mmio_write_features(struct virtio_device *dev, uint64 features) {
    virtio_mmio_data_t *d = dev->transport_data;
    mmio_write32(d->base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(d->base, VIRTIO_MMIO_DRIVER_FEATURES, (uint32)(features & 0xFFFFFFFF));
    mmio_write32(d->base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_write32(d->base, VIRTIO_MMIO_DRIVER_FEATURES, (uint32)(features >> 32));
}

static int mmio_setup_queue(struct virtio_device *dev, uint16 queue_idx,
                             uint16 queue_size, virtq_t *vq) {
    virtio_mmio_data_t *d = dev->transport_data;

    //select the queue index and read the device's max supported size
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_SEL, queue_idx);

    uint16 max = (uint16)mmio_read32(d->base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_size > max) queue_size = max;
    if (queue_size == 0) return -1;
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_NUM, queue_size);

    //calculate layout: descriptor table, available ring (header+ring+flags),
    //and used ring (header+ring+flags) all page-aligned
    size desc_bytes = sizeof(virtq_desc_t) * queue_size;
    size avail_bytes = sizeof(uint16) * (3 + queue_size);
    size used_bytes = sizeof(uint16) * 3 + sizeof(virtq_used_elem_t) * queue_size;
    size total = desc_bytes + avail_bytes + used_bytes;
    size pages = (total + 0xFFF) / 0x1000;

    //allocate contiguous physical memory and map it to the kernel
    void *phys = pmm_alloc(pages);
    if (!phys) return -1;
    void *virt = P2V(phys);
    memset(virt, 0, pages * 0x1000);

    //wire up the virtq pointers into the allocated region
    vq->desc = (virtq_desc_t *)virt;
    vq->avail = (virtq_avail_t *)((uintptr)virt + desc_bytes);
    vq->used = (virtq_used_t *)((uintptr)virt + desc_bytes + avail_bytes);
    vq->queue_idx = queue_idx;

    virtq_init(vq, queue_size);

    //program the device with the physical addresses of each virtq region
    uintptr desc_phys = (uintptr)phys;
    uintptr avail_phys = desc_phys  + desc_bytes;
    uintptr used_phys = avail_phys + avail_bytes;

    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32)(desc_phys & 0xFFFFFFFF));
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32)(desc_phys >> 32));
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32)(avail_phys & 0xFFFFFFFF));
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32)(avail_phys >> 32));
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32)(used_phys & 0xFFFFFFFF));
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,(uint32)(used_phys >> 32));

    //mark the queue as ready, device may start using it immediately
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static void mmio_notify(struct virtio_device *dev, virtq_t *vq) {
    virtio_mmio_data_t *d = dev->transport_data;
    mmio_write32(d->base, VIRTIO_MMIO_QUEUE_NOTIFY, vq->queue_idx);
}

static const virtio_transport_t mmio_transport = {
    .read_cfg8 = mmio_read_cfg8,
    .read_cfg16 = mmio_read_cfg16,
    .read_cfg32 = mmio_read_cfg32,
    .write_cfg8 = mmio_write_cfg8,
    .write_cfg16 = mmio_write_cfg16,
    .write_cfg32 = mmio_write_cfg32,
    .read_status = mmio_read_status,
    .write_status = mmio_write_status,
    .read_features = mmio_read_features,
    .write_features = mmio_write_features,
    .setup_queue= mmio_setup_queue,
    .notify = mmio_notify,
};

//probe a known MMIO base address and register the device if valid
bool virtio_mmio_init_at(uintptr phys_base) {
    //map one page for the register file
    uintptr virt = phys_base + HHDM_OFFSET;
    vmm_kernel_map(virt, phys_base, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    //sanity checks
    uint32 magic = mmio_read32(virt, VIRTIO_MMIO_MAGIC_VALUE);
    uint32 version = mmio_read32(virt, VIRTIO_MMIO_VERSION);
    uint32 dev_id = mmio_read32(virt, VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC) return false;
    if (version != 2) {
        printf("[virtio-mmio] 0x%llX: legacy version %u not supported\n",
               (uint64)phys_base, version);
        return false;
    }
    if (dev_id == 0) return false; //placeholder slot, no device

    virtio_mmio_data_t *d = kzalloc(sizeof(virtio_mmio_data_t));
    if (!d) return false;
    d->base = virt;

    virtio_device_t *dev = kzalloc(sizeof(virtio_device_t));
    if (!dev) { 
        kfree(d); 
        return false; 
    }

    dev->device_type = dev_id;
    dev->transport = &mmio_transport;
    dev->transport_data = d;
    dev->next = virtio_devices;
    virtio_devices = dev;

    printf("[virtio-mmio] registered device type 0x%04X at phys 0x%llX\n",
           dev_id, (uint64)phys_base);
    return true;
}
