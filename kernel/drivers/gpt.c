#include <drivers/gpt.h>
#include <drivers/blkdev.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <lib/io.h>
#include <lib/string.h>
#include <fs/fs.h>

static int part_stat_op(object_t *obj, stat_t *st);

//check if GUID is null
static bool guid_is_null(const uint8 *guid) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) return false;
    }
    return true;
}

//partition read - forwards to parent with offset
static int partition_read(blkdev_t *dev, uint64 lba, uint32 count, void *buf) {
    blkdev_t *parent = dev->parent;
    if (!parent) return -1;
    
    //bounds check
    if (lba + count > dev->sector_count) return -1;
    
    return parent->ops->read(parent, dev->start_lba + lba, count, buf);
}

//partition write - forwards to parent with offset
static int partition_write(blkdev_t *dev, uint64 lba, uint32 count, const void *buf) {
    blkdev_t *parent = dev->parent;
    if (!parent) return -1;
    
    //bounds check
    if (lba + count > dev->sector_count) return -1;
    
    return parent->ops->write(parent, dev->start_lba + lba, count, buf);
}

static blkdev_ops_t partition_ops = {
    .read = partition_read,
    .write = partition_write,
};

//object ops for partition device
static ssize part_read_op(object_t *obj, void *buf, size len, size offset) {
    blkdev_t *dev = (blkdev_t *)obj->data;
    if (!dev) return -1;
    
    if (offset % dev->sector_size != 0) return -1;
    if (len % dev->sector_size != 0) return -1;
    
    uint64 lba = offset / dev->sector_size;
    uint32 count = len / dev->sector_size;
    
    if (lba + count > dev->sector_count) return -1;
    
    //allocate kernel bounce buffer for NVMe DMA
    void *kbuf_phys = pmm_alloc((len + 4095) / 4096);
    if (!kbuf_phys) return -1;
    void *kbuf = P2V(kbuf_phys);
    
    int result = partition_read(dev, lba, count, kbuf);
    if (result == 0) {
        memcpy(buf, kbuf, len);  //copy to userspace
    }
    
    pmm_free(kbuf_phys, (len + 4095) / 4096);
    return (result == 0) ? (ssize)len : -1;
}

static ssize part_write_op(object_t *obj, const void *buf, size len, size offset) {
    blkdev_t *dev = (blkdev_t *)obj->data;
    if (!dev) return -1;
    
    if (offset % dev->sector_size != 0) return -1;
    if (len % dev->sector_size != 0) return -1;
    
    uint64 lba = offset / dev->sector_size;
    uint32 count = len / dev->sector_size;
    
    if (lba + count > dev->sector_count) return -1;
    
    //allocate kernel bounce buffer for NVmE DMA
    void *kbuf_phys = pmm_alloc((len + 4095) / 4096);
    if (!kbuf_phys) return -1;
    void *kbuf = P2V(kbuf_phys);
    
    memcpy(kbuf, buf, len);  //copy from userspace
    int result = partition_write(dev, lba, count, kbuf);
    
    pmm_free(kbuf_phys, (len + 4095) / 4096);
    return (result == 0) ? (ssize)len : -1;
}

static int part_stat_op(object_t *obj, stat_t *st) {
    blkdev_t *dev = (blkdev_t *)obj->data;
    if (!dev || !st) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DEVICE;
    st->size = dev->sector_count * dev->sector_size;
    return 0;
}

static object_ops_t partition_object_ops = {
    .read = part_read_op,
    .write = part_write_op,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = part_stat_op
};

int gpt_scan(blkdev_t *dev) {
    if (!dev || !dev->ops || !dev->ops->read) return -1;
    
    //allocate buffer for reading GPT header (LBA 1)
    void *buf_phys = pmm_alloc(1);
    if (!buf_phys) return -1;
    void *buf = P2V(buf_phys);
    
    //read LBA 1 (GPT header)
    if (dev->ops->read(dev, 1, 1, buf) != 0) {
        pmm_free(buf_phys, 1);
        return -1;
    }
    
    gpt_header_t *hdr = (gpt_header_t *)buf;
    
    //validate signature
    if (hdr->signature != GPT_SIGNATURE) {
        pmm_free(buf_phys, 1);
        return 0; //not GPT not an error
    }
    
    printf("[gpt] Found GPT on %s: header LBA %llu, table LBA %llu, entry count %u\n", 
           dev->name, hdr->my_lba, hdr->partition_entry_lba, hdr->num_partition_entries);
    
    uint32 entry_size = hdr->partition_entry_size;
    uint32 entries_per_sector = dev->sector_size / entry_size;
    uint32 partitions_found = 0;
    
    //scan entries
    for (uint32 i = 0; i < hdr->num_partition_entries && i < 128; i++) {
        uint32 sector_offset = i / entries_per_sector;
        uint32 entry_in_sector = i % entries_per_sector;
        uint64 lba = hdr->partition_entry_lba + sector_offset;
        
        //read sector if we moved to a new one or it's the first
        if (i == 0 || entry_in_sector == 0) {
            if (dev->ops->read(dev, lba, 1, buf) != 0) {
                printf("[gpt] ERR: Failed to read sector %llu\n", lba);
                break;
            }
        }
        
        gpt_entry_t *entry = (gpt_entry_t *)((uint8 *)buf + (entry_in_sector * entry_size));
        
        if (guid_is_null(entry->type_guid)) continue;
        
        //valid partition found
        uint64 start = entry->starting_lba;
        uint64 end = entry->ending_lba;
        uint64 sectors = end - start + 1;
        
        blkdev_t *part = kzalloc(sizeof(blkdev_t));
        if (!part) continue;
        
        char *name = kzalloc(32);
        if (!name) {
            kfree(part);
            continue;
        }
        snprintf(name, 32, "%sp%u", dev->name, partitions_found + 1);
        
        part->name = name;
        part->sector_size = dev->sector_size;
        part->sector_count = sectors;
        part->ops = &partition_ops;
        part->data = dev->data;
        part->parent = dev;
        part->start_lba = start;
        
        object_t *obj = object_create(OBJECT_DEVICE, &partition_object_ops, part);
        if (obj) {
            char ns_name[64];
            snprintf(ns_name, sizeof(ns_name), "$devices/disks/%s", name);
            ns_register(ns_name, obj);
            printf("[gpt]   %s: LBA %llu - %llu (%llu sectors)\n", name, start, end, sectors);
        }
        
        partitions_found++;
    }
    
    printf("[gpt] Scan complete, found %u partitions on %s\n", partitions_found, dev->name);
    
    pmm_free(buf_phys, 1);
    return partitions_found;
}

//generic wrapper for blkdev
int blkdev_scan_partitions(blkdev_t *dev) {
    return gpt_scan(dev);
}
