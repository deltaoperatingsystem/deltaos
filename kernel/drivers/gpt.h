#ifndef DRIVERS_GPT_H
#define DRIVERS_GPT_H

#include <arch/types.h>
#include <drivers/blkdev.h>

#define GPT_SIGNATURE 0x5452415020494645ULL  //"EFI PART"

//GPT header (LBA 1)
typedef struct {
    uint64 signature;
    uint32 revision;
    uint32 header_size;
    uint32 header_crc32;
    uint32 reserved;
    uint64 my_lba;
    uint64 alternate_lba;
    uint64 first_usable_lba;
    uint64 last_usable_lba;
    uint8  disk_guid[16];
    uint64 partition_entry_lba;
    uint32 num_partition_entries;
    uint32 partition_entry_size;
    uint32 partition_entries_crc32;
} __attribute__((packed)) gpt_header_t;

//GPT partition entry (128 bytes each)
typedef struct {
    uint8  type_guid[16];
    uint8  partition_guid[16];
    uint64 starting_lba;
    uint64 ending_lba;
    uint64 attributes;
    uint16 name[36];  //UTF-16LE
} __attribute__((packed)) gpt_entry_t;

//scan for GPT partitions on a block device
//returns number of partitions found or negative on error
int gpt_scan(blkdev_t *dev);

#endif
