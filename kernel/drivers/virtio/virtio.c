#include "virtio.h"
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <lib/io.h>
#include <lib/string.h>
#include <arch/cpu.h>

virtio_device_t *virtio_devices = NULL;

//virtqueue management
void virtq_init(virtq_t *vq, uint16 size) {
    vq->desc_count = size;
    vq->free_head  = 0;
    vq->free_count = size;
    vq->last_used_idx = 0;
    wait_queue_init(&vq->wq);
    spinlock_init(&vq->lock);

    //chain all descriptors together into the free list
    for (uint16 i = 0; i < size - 1; i++) {
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
        vq->desc[i].next  = i + 1;
    }
    vq->desc[size - 1].flags = 0;
    vq->desc[size - 1].next  = 0;

    //driver suppresses used ring interrupts (we use polling/wakeup instead)
    vq->avail->flags = 0;
    vq->avail->idx = 0;
    vq->used->flags = 0;
    vq->used->idx = 0;
}

int virtq_alloc_desc(virtq_t *vq) {
    if (vq->free_count == 0) return -1;

    uint16 idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->free_count--;

    vq->desc[idx].flags = 0;
    vq->desc[idx].next = 0;
    return idx;
}

void virtq_free_desc(virtq_t *vq, uint16 idx) {
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->free_count++;
}

void virtq_kick(virtio_device_t *dev, virtq_t *vq, uint16 head) {
    //write the head index into the available ring and advance the counter
    uint16 slot = vq->avail->idx & (vq->desc_count - 1);
    *(volatile uint16 *)&vq->avail->ring[slot] = head;

    //barrier: all descriptor writes must be visible before we update idx
    __atomic_thread_fence(__ATOMIC_RELEASE);
    
    uint16 next_idx = vq->avail->idx + 1;
    __atomic_store_n(&vq->avail->idx, next_idx, __ATOMIC_RELEASE);

    __atomic_thread_fence(__ATOMIC_RELEASE);
    dev->transport->notify(dev, vq);
}

int virtq_poll_used(virtq_t *vq, uint16 head_idx) {
    //spin until the device posts an entry matching our head descriptor
    //the device can post entries out of order so we scan the whole ring
    for (int retries = 5000000; retries > 0; retries--) {
        uint16 used_idx = __atomic_load_n(&vq->used->idx, __ATOMIC_ACQUIRE);

        while (vq->last_used_idx != used_idx) {
            uint16 slot = vq->last_used_idx & (vq->desc_count - 1);
            virtq_used_elem_t *elem = &vq->used->ring[slot];

            __atomic_thread_fence(__ATOMIC_ACQUIRE);

            uint32 len = elem->len;
            uint32 id = elem->id;
            vq->last_used_idx++;

            if ((uint16)id == head_idx) return (int)len;
        }

        arch_pause();
    }

    return -1; //timeout
}

//device init sequence (virtio spec 3.1.1)
int virtio_dev_init(virtio_device_t *dev, uint64 driver_features) {
    const virtio_transport_t *t = dev->transport;

    //reset
    t->write_status(dev, 0);

    //acknowledge
    t->write_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);

    //driver
    t->write_status(dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    //feature negotiation
    uint64 device_features = t->read_features(dev);
    uint64 negotiated = device_features & driver_features;
    t->write_features(dev, negotiated);

    //features ok
    uint8 status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    t->write_status(dev, status);

    //re-read to confirm the device accepted our features
    if (!(t->read_status(dev) & VIRTIO_STATUS_FEATURES_OK)) {
        printf("[virtio] ERR: device rejected feature set\n");
        t->write_status(dev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    return 0;
}
