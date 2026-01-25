#ifndef DRIVERS_BLKDEV_H
#define DRIVERS_BLKDEV_H

#include <arch/types.h>

//forward declaration
struct blkdev;

//block device operations
typedef struct blkdev_ops {
    //read sectors from device
    //returns 0 on success negative on error
    int (*read)(struct blkdev *dev, uint64 lba, uint32 count, void *buf);
    
    //write sectors to device
    //returns 0 on success nnd negative on error
    int (*write)(struct blkdev *dev, uint64 lba, uint32 count, const void *buf);
} blkdev_ops_t;

//block device
typedef struct blkdev {
    const char *name;           //device name (e.g., "nvme0n1")
    uint32 sector_size;         //bytes per sector (usually 512)
    uint64 sector_count;        //total sectors
    blkdev_ops_t *ops;          //operations
    void *data;                 //driver-specific data
    
    struct blkdev *parent;      //parent device (for partitions)
    uint64 start_lba;           //start LBA (for partitions, 0 for whole disk)
} blkdev_t;

//scan for partitions on a block device and register them
int blkdev_scan_partitions(blkdev_t *dev);

//read helper that handles partition offset
static inline int blkdev_read(blkdev_t *dev, uint64 lba, uint32 count, void *buf) {
    return dev->ops->read(dev, dev->start_lba + lba, count, buf);
}

//write helper that handles partition offset
static inline int blkdev_write(blkdev_t *dev, uint64 lba, uint32 count, const void *buf) {
    return dev->ops->write(dev, dev->start_lba + lba, count, buf);
}

#endif
