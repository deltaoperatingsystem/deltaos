#include "virtio_pci.h"
#include "virtio.h"
#include <drivers/pci.h>
#include <drivers/init.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <arch/mmu.h>
#include <arch/irq.h>
#include <arch/interrupts.h>
#include <lib/io.h>
#include <lib/string.h>

//MSI-X vector base for virtio devices (after NVMe)
#define VIRTIO_MSIX_VECTOR_BASE 0x60
#define VIRTIO_MSIX_MAX_DEVS 4

static uint8 next_msix_vector = VIRTIO_MSIX_VECTOR_BASE;

//MSI-X table entry (same layout as in nvme.h)
typedef struct {
    uint32 msg_addr_low;
    uint32 msg_addr_high;
    uint32 msg_data;
    uint32 vector_control;
} __attribute__((packed)) vio_msix_entry_t;

#define PCI_CAP_ID_MSIX     0x11
#define PCI_CAP_ID_VENDOR   0x09

//read a virtio_pci_cap_t from config space at offset ptr
static void read_cap(pci_device_t *pci, uint8 ptr, virtio_pci_cap_t *cap) {
    uint8 *raw = (uint8 *)cap;
    for (uint32 i = 0; i < sizeof(virtio_pci_cap_t); i++) {
        raw[i] = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                         (uint16)(ptr + i), 1);
    }
}

//map a BAR region into kernel virtual address space (uncached)
static void *map_bar_region(pci_device_t *pci, uint8 bar, uint32 offset, uint32 length) {
    if (bar >= 6 || pci->bar[bar].addr == 0) return NULL;

    uintptr phys = (uintptr)(pci->bar[bar].addr + offset);
    size pages = (length + 0xFFF) / 0x1000;
    void *virt = (void *)(phys + HHDM_OFFSET);
    vmm_kernel_map((uintptr)virt, phys, pages, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);
    return virt;
}

//transport ops
static uint8 pci_read_cfg8(struct virtio_device *dev, uint32 offset) {
    virtio_pci_data_t *d = dev->transport_data;
    return *(volatile uint8 *)((uintptr)d->device_cfg + offset);
}

static uint16 pci_read_cfg16(struct virtio_device *dev, uint32 offset) {
    virtio_pci_data_t *d = dev->transport_data;
    return *(volatile uint16 *)((uintptr)d->device_cfg + offset);
}

static uint32 pci_read_cfg32(struct virtio_device *dev, uint32 offset) {
    virtio_pci_data_t *d = dev->transport_data;
    return *(volatile uint32 *)((uintptr)d->device_cfg + offset);
}

static void pci_write_cfg8(struct virtio_device *dev, uint32 offset, uint8 val) {
    virtio_pci_data_t *d = dev->transport_data;
    *(volatile uint8 *)((uintptr)d->device_cfg + offset) = val;
}

static void pci_write_cfg16(struct virtio_device *dev, uint32 offset, uint16 val) {
    virtio_pci_data_t *d = dev->transport_data;
    *(volatile uint16 *)((uintptr)d->device_cfg + offset) = val;
}

static void pci_write_cfg32(struct virtio_device *dev, uint32 offset, uint32 val) {
    virtio_pci_data_t *d = dev->transport_data;
    *(volatile uint32 *)((uintptr)d->device_cfg + offset) = val;
}

static uint8 pci_read_status(struct virtio_device *dev) {
    virtio_pci_data_t *d = dev->transport_data;
    return d->common->device_status;
}

static void pci_write_status(struct virtio_device *dev, uint8 status) {
    virtio_pci_data_t *d = dev->transport_data;
    d->common->device_status = status;
}

static uint64 pci_read_features(struct virtio_device *dev) {
    virtio_pci_data_t *d = dev->transport_data;
    d->common->device_feature_select = 0;
    uint64 lo = d->common->device_feature;
    d->common->device_feature_select = 1;
    uint64 hi = d->common->device_feature;
    return lo | (hi << 32);
}

static void pci_write_features(struct virtio_device *dev, uint64 features) {
    virtio_pci_data_t *d = dev->transport_data;
    d->common->driver_feature_select = 0;
    d->common->driver_feature = (uint32)(features & 0xFFFFFFFF);
    d->common->driver_feature_select = 1;
    d->common->driver_feature = (uint32)(features >> 32);
}

static int pci_setup_queue(struct virtio_device *dev, uint16 queue_idx,
                            uint16 queue_size, virtq_t *vq) {
    virtio_pci_data_t *d = dev->transport_data;
    volatile virtio_pci_common_cfg_t *cfg = d->common;

    cfg->queue_select = queue_idx;

    //clamp to what the device supports
    uint16 max = cfg->queue_size;
    if (queue_size > max) queue_size = max;
    if (queue_size == 0) return -1;
    cfg->queue_size = queue_size;

    //allocate physically contiguous pages for the three queue regions
    //desc table: 16 * queue_size bytes
    //avail ring: 6 + 2*queue_size bytes
    //used ring: 6 + 8*queue_size bytes (+ padding to align to page)
    size desc_bytes = sizeof(virtq_desc_t) * queue_size;
    size avail_bytes = sizeof(uint16) * (3 + queue_size);  //flags+idx+ring+used_event
    size used_bytes = sizeof(uint16) * 3 + sizeof(virtq_used_elem_t) * queue_size;

    size total = desc_bytes + avail_bytes + used_bytes;
    size pages = (total + 0xFFF) / 0x1000;

    void *phys = pmm_alloc(pages);
    if (!phys) return -1;
    void *virt = P2V(phys);
    memset(virt, 0, pages * 0x1000);

    vq->desc = (virtq_desc_t *)virt;
    vq->avail = (virtq_avail_t *)((uintptr)virt + desc_bytes);
    vq->used = (virtq_used_t  *)((uintptr)virt + desc_bytes + avail_bytes);
    vq->queue_idx = queue_idx;

    virtq_init(vq, queue_size);

    //write physical addresses into the common config
    uintptr desc_phys = (uintptr)phys;
    uintptr avail_phys = desc_phys + desc_bytes;
    uintptr used_phys = avail_phys + avail_bytes;

    cfg->queue_desc = desc_phys;
    cfg->queue_driver = avail_phys;
    cfg->queue_device = used_phys;

    //disable MSI-X vector for this queue (use polling)
    cfg->queue_msix_vector = 0xFFFF;
    //^^  TODO, we should setup a real vector, write an ISR, 
    //and using a wait queue instead of polling, 
    //BUT the GPU driver is 2D-only rn so it doesn't matter AS MUCH but need to rewrite later

    cfg->queue_enable = 1;
    return 0;
}

static void pci_notify(struct virtio_device *dev, virtq_t *vq) {
    virtio_pci_data_t *d = dev->transport_data;
    d->common->queue_select = vq->queue_idx;
    uint16 notify_off = d->common->queue_notify_off;
    uintptr addr = (uintptr)d->notify_base +
                   notify_off * d->notify_off_multiplier;
    //printf("[virtio-pci] notify queue %u at %p (offset %u, mult %u)\n",
    //       vq->queue_idx, (void *)addr, notify_off, d->notify_off_multiplier);
    *(volatile uint16 *)addr = vq->queue_idx;
}

static const virtio_transport_t pci_transport = {
    .read_cfg8 = pci_read_cfg8,
    .read_cfg16 = pci_read_cfg16,
    .read_cfg32 = pci_read_cfg32,
    .write_cfg8 = pci_write_cfg8,
    .write_cfg16 = pci_write_cfg16,
    .write_cfg32 = pci_write_cfg32,
    .read_status = pci_read_status,
    .write_status = pci_write_status,
    .read_features = pci_read_features,
    .write_features = pci_write_features,
    .setup_queue = pci_setup_queue,
    .notify = pci_notify,
};

//set up MSI-X for the control queue interrupt
static void virtio_pci_setup_msix(pci_device_t *pci, virtio_pci_data_t *d) {
    if (!(pci->status & (1 << 4))) return; //no capabilities list

    uint8 ptr = (uint8)pci_config_read(pci->bus, pci->dev, pci->func, 0x34, 1);
    while (ptr) {
        uint8 id = (uint8)pci_config_read(pci->bus, pci->dev, pci->func, ptr, 1);
        if (id == PCI_CAP_ID_MSIX) {
            d->msix_cap_ptr = ptr;
            break;
        }
        ptr = (uint8)pci_config_read(pci->bus, pci->dev, pci->func, ptr + 1, 1);
    }

    if (!d->msix_cap_ptr) return;

    uint32 table_info = pci_config_read(pci->bus, pci->dev, pci->func,
                                         d->msix_cap_ptr + 4, 4);
    uint8  bir = table_info & 0x7;
    uint32 offset = table_info & ~0x7U;

    uintptr phys = (uintptr)(pci->bar[bir].addr + offset);
    d->msix_table = (void *)(phys + HHDM_OFFSET);
    vmm_kernel_map((uintptr)d->msix_table, phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    if (next_msix_vector >= VIRTIO_MSIX_VECTOR_BASE + VIRTIO_MSIX_MAX_DEVS * 2) return;

    uint8 vec = next_msix_vector++;
    irq_msi_msg_t msg;
    if (irq_compose_msi(vec, &msg) < 0) return;

    volatile vio_msix_entry_t *entry = (volatile vio_msix_entry_t *)d->msix_table;
    entry->msg_addr_low = msg.addr_lo;
    entry->msg_addr_high = msg.addr_hi;
    entry->msg_data = msg.data;
    entry->vector_control = 0;  //unmask

    //enable MSI-X in the message control register
    uint16 msg_ctrl = (uint16)pci_config_read(pci->bus, pci->dev, pci->func,
                                               d->msix_cap_ptr + 2, 2);
    pci_config_write(pci->bus, pci->dev, pci->func,
                     d->msix_cap_ptr + 2, 2, msg_ctrl | (1 << 15));

    d->msix_vector = vec;
    d->msix_ok     = true;
    printf("[virtio-pci] MSI-X vector 0x%02X allocated\n", vec);
}

//probe one PCI device and register it as a virtio device if applicable
static void virtio_pci_probe(pci_device_t *pci) {
    //virtio devices use vendor 0x1AF4 and device IDs 0x1040-0x107F (modern)
    if (pci->vendor_id != VIRTIO_PCI_VENDOR) return;
    if (pci->device_id < 0x1040 || pci->device_id > 0x107F) return;

    virtio_pci_data_t *d = kzalloc(sizeof(virtio_pci_data_t));
    if (!d) return;

    d->pci = pci;
    pci_enable_mmio(pci);
    pci_enable_bus_master(pci);

    //walk the PCI capability list looking for virtio vendor caps
    if (!(pci->status & (1 << 4))) goto no_caps;

    uint8 ptr = (uint8)pci_config_read(pci->bus, pci->dev, pci->func, 0x34, 1);
    while (ptr) {
        uint8 id = (uint8)pci_config_read(pci->bus, pci->dev, pci->func, ptr, 1);
        if (id == PCI_CAP_ID_VENDOR) {
            virtio_pci_cap_t cap;
            read_cap(pci, ptr, &cap);

            switch (cap.cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    d->common = map_bar_region(pci, cap.bar, cap.offset, cap.length);
                    break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG: {
                    d->notify_base = map_bar_region(pci, cap.bar, cap.offset, cap.length);
                    //the multiplier sits immediately after the cap struct
                    d->notify_off_multiplier = pci_config_read(
                        pci->bus, pci->dev, pci->func,
                        (uint16)(ptr + sizeof(virtio_pci_cap_t)), 4);
                    break;
                }
                case VIRTIO_PCI_CAP_ISR_CFG:
                    d->isr = map_bar_region(pci, cap.bar, cap.offset, cap.length);
                    break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    d->device_cfg = map_bar_region(pci, cap.bar, cap.offset, cap.length);
                    break;
                default:
                    break;
            }
        }
        ptr = (uint8)pci_config_read(pci->bus, pci->dev, pci->func, ptr + 1, 1);
    }

no_caps:
    if (!d->common) {
        printf("[virtio-pci] %02X:%02X.%X no common cfg, skipping\n",
               pci->bus, pci->dev, pci->func);
        kfree(d);
        return;
    }

    //virtio modern: device type is (PCI Device ID - 0x1040)
    uint16 device_type = pci->device_id - 0x1040;

    virtio_device_t *dev = kzalloc(sizeof(virtio_device_t));
    if (!dev) { kfree(d); return; }

    dev->device_type = device_type;
    dev->transport = &pci_transport;
    dev->transport_data = d;

    //set up MSI-X if the device has it
    virtio_pci_setup_msix(pci, d);

    //prepend to global list
    dev->next = virtio_devices;
    virtio_devices = dev;

    printf("[virtio-pci] registered device type %u at %02X:%02X.%X\n",
           device_type, pci->bus, pci->dev, pci->func);
}

void virtio_pci_init(void) {
    for (pci_device_t *pci = pci_get_devices(); pci; pci = pci->next) {
        virtio_pci_probe(pci);
    }
}

DECLARE_DRIVER(virtio_pci_init, INIT_LEVEL_BUS);
