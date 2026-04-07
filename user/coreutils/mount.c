#include <system.h>
#include <io.h>
#include <string.h>
#include <isoc/stdio.h>

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

//quick boot sector sanity check
static int looks_like_fat32(handle_t source) {
    block_device_info_t info = {0};
    uint32 sector_size = 512;

    if (object_get_info(source, OBJ_INFO_BLOCK_DEVICE, &info, sizeof(info)) == 0 &&
        info.sector_size >= sizeof(fat32_boot_sector_t) &&
        info.sector_size <= 4096) {
        sector_size = info.sector_size;
    }

    //read one sector into a fixed buffer
    uint8 sector[4096] = {0};
    if (sector_size > sizeof(sector)) {
        return 0;
    }

    if (handle_seek(source, 0, HANDLE_SEEK_SET) < 0) {
        return 0;
    }

    if (handle_read(source, sector, sector_size) != (int)sector_size) {
        return 0;
    }

    fat32_boot_sector_t *bs = (fat32_boot_sector_t *)sector;

    if (bs->bytes_per_sector == 0 || bs->sectors_per_cluster == 0) {
        return 0;
    }

    if (bs->fat_count == 0 || bs->fat_size_32 == 0 || bs->root_cluster < 2) {
        return 0;
    }

    if (bs->boot_sig != 0x29) {
        return 0;
    }

    if (memcmp(bs->fs_type, "FAT32   ", 8) != 0) {
        return 0;
    }

    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        puts("Usage: mount <source> <target> [fstype]\n");
        puts("Example: mount $devices/disks/nvme0n1 /mnt/disk fat32\n");
        return 1;
    }

    //default to fat32
    char *fstype = (argc >= 4) ? argv[3] : "fat32";
    //open the source device with read write and info rights
    handle_t source = get_obj(INVALID_HANDLE, argv[1], RIGHT_READ | RIGHT_WRITE | RIGHT_GET_INFO);
    if (source == INVALID_HANDLE) {
        printf("mount: cannot open '%s'\n", argv[1]);
        return 1;
    }

    //cheap check so we fail fast on obvious non fat32 disks
    if (strcmp(fstype, "fat32") == 0 && !looks_like_fat32(source)) {
        fprintf(stderr, "mount: warning: '%s' does not look like a FAT32 volume; continuing anyway\n", argv[1]);
    }

    //hand the mount request to the kernel side
    int rc = mount(source, argv[2], fstype);
    handle_close(source);

    if (rc < 0) {
        printf("mount: failed to mount %s at %s (error %d)\n", argv[1], argv[2], rc);
        return 1;
    }

    printf("mount: mounted %s at %s as %s\n", argv[1], argv[2], fstype);
    return 0;
}
