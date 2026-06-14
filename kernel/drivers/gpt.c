#include <drivers/gpt.h>
#include <drivers/blkdev.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/crc32.h>
#include <fs/fs.h>
#include <syscall/syscall.h>

static int part_stat_op(object_t *obj, stat_t *st);
static intptr part_get_info_op(object_t *obj, uint32 topic, void *buf, size len);

//check if GUID is null
static bool guid_is_null(const uint8 *guid) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) return false;
    }
    return true;
}

//partition read - forwards to parent block device with LBA offset applied
static int partition_read(blkdev_t *dev, uint64 lba, uint32 count, void *buf) {
    blkdev_t *parent = dev->parent;
    if (!parent) return -1;
    
    //bounds check against partition size we use subtraction form to avoid lba+count wrap
    if (lba >= dev->sector_count || (uint64)count > dev->sector_count - lba) return -1;
    
    return parent->ops->read(parent, dev->start_lba + lba, count, buf);
}

//partition write - forwards to parent block device with LBA offset applied
static int partition_write(blkdev_t *dev, uint64 lba, uint32 count, const void *buf) {
    blkdev_t *parent = dev->parent;
    if (!parent) return -1;
    
    //bounds check against partition size we use subtraction form to avoid lba+count wrap
    if (lba >= dev->sector_count || (uint64)count > dev->sector_count - lba) return -1;
    
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
    
    //bounds check against partition size we use subtraction form to avoid lba+count wrap
    if (lba >= dev->sector_count || (uint64)count > dev->sector_count - lba) return -1;
    
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
    
    //bounds check against partition size we use subtraction form to avoid lba+count wrap
    if (lba >= dev->sector_count || (uint64)count > dev->sector_count - lba) return -1;
    
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

static intptr part_get_info_op(object_t *obj, uint32 topic, void *buf, size len) {
    blkdev_t *dev = (blkdev_t *)obj->data;
    if (!dev || !buf) return -1;

    if (topic == OBJ_INFO_BLOCK_DEVICE) {
        if (len < sizeof(block_device_info_t)) return -1;
        block_device_info_t info;
        info.sector_size = dev->sector_size;
        info.sector_count = dev->sector_count;
        memcpy(buf, &info, sizeof(info));
        return 0;
    }
    return -1;
}

static object_ops_t partition_object_ops = {
    .read = part_read_op,
    .write = part_write_op,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = part_stat_op,
    .get_info = part_get_info_op
};

int gpt_scan(blkdev_t *dev) {
    if (!dev || !dev->ops || !dev->ops->read) return -1;
    
    //allocate one page for all sector I/O
    void *buf_phys = pmm_alloc(1);
    if (!buf_phys) return -1;
    void *buf = P2V(buf_phys);
    
    //read LBA 1 (GPT header)
    if (dev->ops->read(dev, 1, 1, buf) != 0) {
        pmm_free(buf_phys, 1);
        return -1;
    }
    
    gpt_header_t *hdr = (gpt_header_t *)buf;
    
    //validate magic signature
    if (hdr->signature != GPT_SIGNATURE) {
        pmm_free(buf_phys, 1);
        return 0; //not GPT not an error
    }

    //validate header_size before we attempt a CRC over it
    if (hdr->header_size < 92 || hdr->header_size > dev->sector_size) {
        printf("[gpt] ERR: %s has invalid header_size=%u, ignoring\n",
               dev->name, hdr->header_size);
        pmm_free(buf_phys, 1);
        return 0;
    }

    //verify header CRC32: zero the crc field, compute over header, restore
    uint32 stored_crc   = hdr->header_crc32;
    hdr->header_crc32   = 0;
    uint32 computed_crc = crc32(hdr, hdr->header_size);
    hdr->header_crc32   = stored_crc;

    if (computed_crc != stored_crc) {
        printf("[gpt] ERR: %s header CRC mismatch (stored=0x%08x computed=0x%08x), ignoring\n",
               dev->name, stored_crc, computed_crc);
        pmm_free(buf_phys, 1);
        return 0;
    }

    //entry size must be a power-of-two in range [128, 4096]
    uint32 entry_size = hdr->partition_entry_size;
    if (entry_size < 128 || entry_size > 4096 || (entry_size & (entry_size - 1)) != 0) {
        printf("[gpt] ERR: %s has invalid partition_entry_size=%u, ignoring\n",
               dev->name, entry_size);
        pmm_free(buf_phys, 1);
        return 0;
    }

    if (dev->sector_size == 0 || entry_size > dev->sector_size) {
        printf("[gpt] ERR: %s has invalid partition_entry_size=%u, ignoring\n",
               dev->name, entry_size);
        pmm_free(buf_phys, 1);
        return 0;
    }

    uint64 orig_num_entries = hdr->num_partition_entries;

    //validate the entry table fits within the disk
    uint64 entries_per_sector = dev->sector_size / entry_size;
    if (entries_per_sector == 0) {
        pmm_free(buf_phys, 1);
        return 0;
    }
    uint64 table_sectors = (orig_num_entries + entries_per_sector - 1) / entries_per_sector;
    if (hdr->partition_entry_lba < 2 ||
        (uint64)hdr->partition_entry_lba + table_sectors > dev->sector_count) {
        printf("[gpt] ERR: %s partition entry table is out of disk bounds, ignoring\n", dev->name);
        pmm_free(buf_phys, 1);
        return 0;
    }

    //allocate and read contiguous buffer for partition entries table CRC check
    uint64 table_size = orig_num_entries * (uint64)entry_size;
    uint64 table_sectors_to_read = (table_size + dev->sector_size - 1) / dev->sector_size;
    uint64 alloc_size = table_sectors_to_read * dev->sector_size;
    if (alloc_size > (uint64)0xFFFFFFFF) {
        printf("[gpt] ERR: %s partition entry table too large, ignoring\n", dev->name);
        pmm_free(buf_phys, 1);
        return 0;
    }
    uint32 table_pages = (uint32)((alloc_size + 4095) / 4096);
    if (table_pages == 0) {
        pmm_free(buf_phys, 1);
        return 0;
    }
    void *table_buf_phys = pmm_alloc(table_pages);
    if (!table_buf_phys) {
        pmm_free(buf_phys, 1);
        return -1;
    }
    void *table_buf = P2V(table_buf_phys);

    if (dev->ops->read(dev, hdr->partition_entry_lba, (uint32)table_sectors_to_read, table_buf) != 0) {
        printf("[gpt] ERR: %s failed to read partition entry table\n", dev->name);
        pmm_free(table_buf_phys, table_pages);
        pmm_free(buf_phys, 1);
        return 0;
    }

    //verify CRC32 over the entries buffer
    uint32 computed_table_crc = crc32(table_buf, table_size);
    if (computed_table_crc != hdr->partition_entries_crc32) {
        printf("[gpt] ERR: %s partition entry table CRC mismatch (stored=0x%08x computed=0x%08x), ignoring\n",
               dev->name, hdr->partition_entries_crc32, computed_table_crc);
        pmm_free(table_buf_phys, table_pages);
        pmm_free(buf_phys, 1);
        return 0;
    }

    //clamp entry count to spec maximum of 128
    uint32 num_entries = orig_num_entries;
    if (num_entries > 128) {
        printf("[gpt] WARN: %s claims %u entries, clamping to 128\n", dev->name, num_entries);
        num_entries = 128;
    }

    printf("[gpt] Found GPT on %s: header LBA %llu, table LBA %llu, entry count %u\n", 
           dev->name, hdr->my_lba, hdr->partition_entry_lba, num_entries);

    uint32 partitions_found = 0;

    //scan partition entries
    for (uint32 i = 0; i < num_entries; i++) {
        gpt_entry_t *entry = (gpt_entry_t *)((uint8 *)table_buf + (i * entry_size));

        //skip empty slots
        if (guid_is_null(entry->type_guid)) continue;

        uint64 start   = entry->starting_lba;
        uint64 end     = entry->ending_lba;

        //validate LBA range before registering this partition
        if (start == 0 || end < start || end >= dev->sector_count) {
            printf("[gpt]   entry %u: invalid LBA range %llu..%llu, skipping\n", i, start, end);
            continue;
        }

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
        part->data = part; //self-pointer: part_read/write_op receive the partition blkdev
        part->parent = dev;
        part->start_lba = start;

        object_t *obj = object_create(OBJECT_DEVICE, &partition_object_ops, part);
        if (obj) {
            char ns_name[64];
            snprintf(ns_name, sizeof(ns_name), "$devices/disks/%s", name);
            ns_register(ns_name, obj, HANDLE_RIGHTS_ALL);
            printf("[gpt]   %s: LBA %llu - %llu (%llu sectors)\n", name, start, end, sectors);
            partitions_found++;
        } else {
            kfree(name);
            kfree(part);
        }
    }

    printf("[gpt] Scan complete, found %u partitions on %s\n", partitions_found, dev->name);

    pmm_free(table_buf_phys, table_pages);
    pmm_free(buf_phys, 1);
    return partitions_found;
}

//generic wrapper for blkdev
int blkdev_scan_partitions(blkdev_t *dev) {
    return gpt_scan(dev);
}

//remove all partitions for a block device from the namespace and re-scan
int gpt_rescan(blkdev_t *dev) {
    if (!dev || !dev->name) return -1;

    //try to unregister up to 128 partitions
    char name[64];
    for (int i = 1; i <= 128; i++) {
        snprintf(name, sizeof(name), "$devices/disks/%sp%u", dev->name, i);
        ns_unregister(name);
    }

    return gpt_scan(dev);
}
