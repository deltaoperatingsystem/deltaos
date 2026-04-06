#include <system.h>
#include <io.h>
#include <mem.h>
#include <string.h>

#define FAT32_MIN_CLUSTERS 65525u
#define FAT32_MAX_CLUSTER_SIZE 32768u

typedef struct __attribute__((packed)) {
    uint8 jump[3];
    char oem[8];              //OEM string
    uint16 bytes_per_sector;
    uint8 sectors_per_cluster;
    uint16 reserved_sectors;
    uint8 fat_count;
    uint16 root_entry_count;   //FAT16 only
    uint16 total_sectors_16;   //FAT16 only
    uint8 media;               //usually f8
    uint16 fat_size_16;        //FAT16 only
    uint16 sectors_per_track;
    uint16 heads;
    uint32 hidden_sectors;     //partition lba
    uint32 total_sectors_32;
    uint32 fat_size_32;        //FAT size in sectors
    uint16 ext_flags;          //mirroring flags
    uint16 fs_version;         //should be 0
    uint32 root_cluster;       //root dir start cluster
    uint16 fsinfo_sector;      //fsinfo sector
    uint16 backup_boot_sector;
    uint8 reserved[12];        //firmware padding
    uint8 drive_number;        //int13h drive number
    uint8 reserved1;
    uint8 boot_sig;            //29 means the extra fields are valid
    uint32 volume_id;          //volume serial
    char volume_label[11];     //11 byte label, space padded
    char fs_type[8];           //usually FAT32
} fat32_boot_sector_t;

typedef struct __attribute__((packed)) {
    uint32 lead_sig;
    uint8 reserved1[480];      //unused space
    uint32 struct_sig;
    uint32 free_clusters;      //best guess only
    uint32 next_free;          //allocation hint
    uint8 reserved2[12];
    uint16 trail_sig;          //aa55
    uint8 reserved3[2];
} fat32_fsinfo_t;

//layout values we derive after probing the disk
typedef struct {
    uint32 sectors_per_cluster;
    uint32 fat_size_sectors;
    uint32 total_clusters;
} fat32_layout_t;

//fill a fixed 11 byte label field
static void fill_label(char out[11], const char *label) {
    memset(out, ' ', 11);
    if (!label || !*label) {
        memcpy(out, "DELTAOS", 7);
        return;
    }

    size len = strlen(label);
    if (len > 11) len = 11;
    for (size i = 0; i < len; i++) {
        char c = label[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
}

//write the full buffer or fail
static int write_all(handle_t h, const void *buf, size len) {
    const uint8 *p = (const uint8 *)buf;
    size done = 0;
    while (done < len) {
        int w = handle_write(h, p + done, (int)(len - done));
        if (w <= 0) return -1;
        done += (size)w;
    }
    return 0;
}

//write one whole sector at lba
static int write_sector(handle_t h, uint64 sector_size, uint64 lba, const void *buf) {
    uint64 offset = lba * sector_size;
    if (handle_seek(h, offset, HANDLE_SEEK_SET) < 0) return -1;
    return write_all(h, buf, (size)sector_size);
}

static void write_u32_le(uint8 *dst, uint32 value) {
    dst[0] = (uint8)(value & 0xFF);
    dst[1] = (uint8)((value >> 8) & 0xFF);
    dst[2] = (uint8)((value >> 16) & 0xFF);
    dst[3] = (uint8)((value >> 24) & 0xFF);
}

//fat entries are 32 bits wide
static void write_fat_entry(uint8 *fat, uint32 index, uint32 value) {
    write_u32_le(fat + index * 4u, value & 0x0FFFFFFFu);
}

//pick a cluster size and fat size that fit the device
static int choose_layout(uint64 total_sectors, uint32 sector_size, fat32_layout_t *layout) {
    static const uint32 candidates[] = {64, 32, 16, 8, 4, 2, 1};
    const uint32 reserved_sectors = 32; //boot, fsinfo, backup, padding
    const uint32 fat_count = 2;         //primary and backup fat

    for (size i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uint32 spc = candidates[i];
        if ((uint64)spc * sector_size > FAT32_MAX_CLUSTER_SIZE) continue;

        uint64 data_sectors = (total_sectors > reserved_sectors) ? (total_sectors - reserved_sectors) : 0;
        uint32 fat_sectors = 0;
        uint32 clusters = 0;

        for (int iter = 0; iter < 12; iter++) {
            //iterate until fat size and data size stop moving
            clusters = (uint32)(data_sectors / spc);
            if (clusters < 2) break;

            uint64 fat_bytes = (uint64)(clusters + 2u) * 4u;
            uint32 next_fat_sectors = (uint32)((fat_bytes + sector_size - 1u) / sector_size);
            uint64 next_data_sectors = (total_sectors > (reserved_sectors + (uint64)fat_count * next_fat_sectors))
                                     ? (total_sectors - reserved_sectors - (uint64)fat_count * next_fat_sectors)
                                     : 0;

            if (next_fat_sectors == fat_sectors && next_data_sectors == data_sectors) {
                fat_sectors = next_fat_sectors;
                clusters = (uint32)(data_sectors / spc);
                break;
            }

            fat_sectors = next_fat_sectors;
            data_sectors = next_data_sectors;
        }

        if (clusters < FAT32_MIN_CLUSTERS) continue;
        if (clusters > 0x0FFFFFF5u) continue;
        if (fat_sectors == 0) continue;

        layout->sectors_per_cluster = spc;
        layout->fat_size_sectors = fat_sectors;
        layout->total_clusters = clusters;
        return 0;
    }

    return -1;
}

static int format_fat32(handle_t dev, uint32 sector_size, uint64 sector_count, const char *label) {
    fat32_layout_t layout;
    if (choose_layout(sector_count, sector_size, &layout) < 0) {
        printf("mkfsfat32: device is too small for FAT32 or no valid cluster size fits\n");
        return -1;
    }

    const uint32 reserved_sectors = 32;
    const uint32 fat_count = 2;
    const uint32 root_cluster = 2;
    uint32 fat_sectors = layout.fat_size_sectors;
    uint32 spc = layout.sectors_per_cluster;
    uint32 cluster_count = layout.total_clusters;
    uint64 data_sectors = sector_count - reserved_sectors - (uint64)fat_count * fat_sectors;
    uint64 cluster_area_sectors = (uint64)cluster_count * spc;
    if (cluster_area_sectors > data_sectors) {
        cluster_count = (uint32)(data_sectors / spc);
    }

    if (sector_size > 4096) {
        printf("mkfsfat32: unsupported sector size %u\n", sector_size);
        return -1;
    }

    uint8 *sector = malloc(sector_size);
    uint8 *fat_sector = malloc(sector_size);
    uint8 *zero = malloc(sector_size);
    if (!sector || !fat_sector || !zero) {
        free(sector);
        free(fat_sector);
        free(zero);
        printf("mkfsfat32: out of memory\n");
        return -1;
    }

    //boot sector and backup copy
    memset(sector, 0, sector_size);
    fat32_boot_sector_t *bpb = (fat32_boot_sector_t *)sector;
    bpb->jump[0] = 0xEB;
    bpb->jump[1] = 0x58;
    bpb->jump[2] = 0x90;
    memcpy(bpb->oem, "DELTAOS ", 8);
    bpb->bytes_per_sector = (uint16)sector_size;
    bpb->sectors_per_cluster = (uint8)spc;
    bpb->reserved_sectors = reserved_sectors;
    bpb->fat_count = fat_count;
    bpb->root_entry_count = 0;
    bpb->total_sectors_16 = 0;
    bpb->media = 0xF8;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 63;
    bpb->heads = 255;
    bpb->hidden_sectors = 0;
    bpb->total_sectors_32 = (sector_count > 0xFFFFu) ? (uint32)sector_count : 0;
    bpb->fat_size_32 = fat_sectors;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = root_cluster;
    bpb->fsinfo_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_sig = 0x29;
    bpb->volume_id = (uint32)(get_ticks() ^ (uint64)getpid());
    fill_label(bpb->volume_label, label);
    memcpy(bpb->fs_type, "FAT32   ", 8);

    if (write_sector(dev, sector_size, 0, sector) < 0) return -1;
    if (write_sector(dev, sector_size, 6, sector) < 0) return -1;

    //fsinfo block
    fat32_fsinfo_t fsinfo;
    memset(&fsinfo, 0, sizeof(fsinfo));
    fsinfo.lead_sig = 0x41615252u;
    fsinfo.struct_sig = 0x61417272u;
    fsinfo.free_clusters = (cluster_count > 1) ? (cluster_count - 1u) : 0;
    fsinfo.next_free = 3;
    fsinfo.trail_sig = 0xAA55;
    memset(sector, 0, sector_size);
    memcpy(sector, &fsinfo, sizeof(fsinfo));
    if (write_sector(dev, sector_size, 1, sector) < 0) return -1;

    //init both fat copies
    uint32 entries_per_sector = sector_size / 4u;
    for (uint32 fat_copy = 0; fat_copy < fat_count; fat_copy++) {
        for (uint32 sec = 0; sec < fat_sectors; sec++) {
            memset(fat_sector, 0, sector_size);
            uint32 base_entry = sec * entries_per_sector;
            for (uint32 i = 0; i < entries_per_sector; i++) {
                uint32 idx = base_entry + i;
                uint32 value = 0;
                if (idx == 0) {
                    //media and end of chain marker
                    value = 0x0FFFFFF8u;
                } else if (idx == 1) {
                    //reserved fat entry
                    value = 0x0FFFFFFFu;
                } else if (idx == root_cluster) {
                    //root dir chain starts here
                    value = 0x0FFFFFFFu;
                }
                write_fat_entry(fat_sector, i, value);
            }

            uint64 lba = reserved_sectors + (uint64)fat_copy * fat_sectors + sec;
            if (write_sector(dev, sector_size, lba, fat_sector) < 0) return -1;
        }
    }

    //zero the root directory cluster
    memset(zero, 0, sector_size);
    uint64 root_lba = reserved_sectors + (uint64)fat_count * fat_sectors + (uint64)(root_cluster - 2u) * spc;
    for (uint32 sec = 0; sec < spc; sec++) {
        if (write_sector(dev, sector_size, root_lba + sec, zero) < 0) return -1;
    }

    printf("mkfsfat32: formatted %llu sectors at %u bytes/sector, %u sectors/cluster\n",
           sector_count, sector_size, spc);
    printf("mkfsfat32: FAT size %u sectors, clusters %u, label '%s'\n",
           fat_sectors, cluster_count, label ? label : "DELTAOS");

    free(sector);
    free(fat_sector);
    free(zero);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("Usage: mkfsfat32 <device> [label]\n");
        puts("Example: mkfsfat32 $devices/disks/nvme0n1p1 DELTAOS\n");
        return 1;
    }

    const char *device = argv[1];
    //default to deltaos if no label is given
    const char *label = (argc >= 3) ? argv[2] : "DELTAOS";

    handle_t dev = get_obj(INVALID_HANDLE, device, RIGHT_READ | RIGHT_WRITE | RIGHT_GET_INFO);
    if (dev == INVALID_HANDLE) {
        printf("mkfsfat32: cannot open '%s'\n", device);
        return 1;
    }

    block_device_info_t info = {0};
    if (object_get_info(dev, OBJ_INFO_BLOCK_DEVICE, &info, sizeof(info)) < 0 || info.sector_size == 0 || info.sector_count == 0) {
        printf("mkfsfat32: '%s' is not a block device\n", device);
        handle_close(dev);
        return 1;
    }

    if (info.sector_size < 512 || info.sector_size > 4096 || (info.sector_size & (info.sector_size - 1)) != 0) {
        printf("mkfsfat32: unsupported sector size %u\n", info.sector_size);
        handle_close(dev);
        return 1;
    }

    if (format_fat32(dev, info.sector_size, info.sector_count, label) < 0) {
        handle_close(dev);
        return 1;
    }

    handle_close(dev);
    return 0;
}
