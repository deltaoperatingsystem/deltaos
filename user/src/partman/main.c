#include <curses.h>
#include <io.h>
#include <mem.h>
#include <string.h>
#include <system.h>
#include <crc32.h>

#define MAX_DEVICES 32
#define MAX_PARTITIONS 128
#define PATH_MAX_LEN 96

#define GPT_SIGNATURE 0x5452415020494645ULL

//gpt header at lba 1
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
    uint8 disk_guid[16];
    uint64 partition_entry_lba;
    uint32 num_partition_entries;
    uint32 partition_entry_size;
    uint32 partition_entries_crc32;
} __attribute__((packed)) gpt_header_t;

//one gpt entry
typedef struct {
    uint8 type_guid[16];
    uint8 partition_guid[16];
    uint64 starting_lba;
    uint64 ending_lba;
    uint64 attributes;
    uint16 name[36];
} __attribute__((packed)) gpt_entry_t;

//one block device we found under $devices/disks
typedef struct {
    char name[64];
    char path[PATH_MAX_LEN];
    block_device_info_t info;
} device_entry_t;

//what we show in the right pane for one partition
typedef struct {
    bool valid;
    char name[40];
    char type_name[32];
    char type_guid[40];
    char guid[40];
    uint64 start_lba;
    uint64 end_lba;
    uint64 sectors;
    uint64 size_bytes;
    uint64 attributes;
    uint32 raw_index;  //index into cached_entries[]
} partition_info_t;

//left pane or right pane
typedef enum {
    FOCUS_DEVICES = 0,
    FOCUS_PARTITIONS = 1,
} focus_pane_t;

//ui state and cached scan results
static device_entry_t devices[MAX_DEVICES];
static uint32 device_count = 0;
static uint32 selected_device = 0;
static partition_info_t partitions[MAX_PARTITIONS];
static uint32 partition_count = 0;
static uint32 selected_partition = 0;
static focus_pane_t focus_pane = FOCUS_DEVICES;
static bool selected_is_gpt = false;
static char detail_title[64] = "";
static char detail_status[96] = "";

//cached live gpt state (valid when selected_is_gpt)
static gpt_header_t cached_gpt_hdr;
static gpt_entry_t  cached_entries[128]; //always 128-entry table
static uint32       cached_num_entries = 0;

//field indices for the new-partition dialog
#define NP_FIELD_START  0
#define NP_FIELD_SIZE   1
#define NP_FIELD_TYPE   2
#define NP_FIELD_NAME   3
#define NP_FIELD_COUNT  4

typedef struct {
    uint8 guid[16];
    const char *name;
} guid_map_t;

static const guid_map_t type_guid_map[] = {
    { { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }, "Linux filesystem" },
    { { 0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
        0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F }, "Linux swap" },
    { { 0x79, 0xD3, 0xD6, 0xE6, 0x07, 0xF5, 0xC2, 0x44,
        0xA2, 0x3C, 0x23, 0x8F, 0x2A, 0x3D, 0xF9, 0x28 }, "Linux LVM" },
    { { 0x0F, 0x88, 0x9D, 0xA1, 0xFC, 0x05, 0x3B, 0x4D,
        0xA0, 0x06, 0x74, 0x3F, 0x0F, 0x84, 0x91, 0x1E }, "Linux RAID" },
    { { 0xE1, 0xC7, 0x3A, 0x93, 0xB4, 0x2E, 0x13, 0x4F,
        0xB8, 0x44, 0x0E, 0x14, 0xE2, 0xAE, 0xF9, 0x15 }, "Linux /home" },
    { { 0xE3, 0xBC, 0x68, 0x4F, 0xCD, 0xE8, 0xB1, 0x4D,
        0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09 }, "Linux x86-64 root" },
    { { 0x45, 0xB0, 0x21, 0xB9, 0xF0, 0x1D, 0xC3, 0x41,
        0xAF, 0x44, 0x4C, 0x6F, 0x28, 0x0D, 0x3F, 0xAE }, "Linux ARM64 root" },
    { { 0xCB, 0x7C, 0x7D, 0xCA, 0xED, 0x63, 0x53, 0x4C,
        0x86, 0x1C, 0x17, 0x42, 0x53, 0x60, 0x59, 0xCC }, "Linux dm-crypt" },
    { { 0xFF, 0xC2, 0x13, 0xBC, 0xE6, 0x59, 0x62, 0x42,
        0xA3, 0x52, 0xB2, 0x75, 0xFD, 0x6F, 0x71, 0x72 }, "Linux extended boot" },
    { { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
        0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 }, "BIOS boot" },
    { { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }, "EFI System" },
    { { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }, "Microsoft Basic Data" },
    { { 0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
        0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE }, "Microsoft Reserved" },
    { { 0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D,
        0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC }, "Windows Recovery" },
    { { 0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
        0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC }, "Apple APFS" },
    { { 0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11,
        0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC }, "Apple HFS+ / Mac" },
    { { 0xB4, 0x7C, 0x6E, 0x51, 0xCF, 0x6E, 0xD6, 0x11,
        0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B }, "FreeBSD UFS" },
    { { 0xB5, 0x7C, 0x6E, 0x51, 0xCF, 0x6E, 0xD6, 0x11,
        0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B }, "FreeBSD ZFS" },
    { { 0xB6, 0x7C, 0x6E, 0x51, 0xCF, 0x6E, 0xD6, 0x11,
        0x8F, 0xF8, 0x00, 0x02, 0x2D, 0x09, 0x71, 0x2B }, "FreeBSD swap" },
};
static const int type_guid_count = (int)(sizeof(type_guid_map) / sizeof(type_guid_map[0]));

//all zero guid means unused entry
static bool guid_is_zero(const uint8 *guid) {
    for (uint32 i = 0; i < 16; i++) {
        if (guid[i] != 0) return false;
    }
    return true;
}

static void guid_to_string(const uint8 *guid, char *buf, size bufsz) {
    snprintf(buf, bufsz,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             guid[3], guid[2], guid[1], guid[0],
             guid[5], guid[4],
             guid[7], guid[6],
             guid[8], guid[9],
             guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

static const char *type_guid_name(const uint8 *guid) {
    for (int i = 0; i < type_guid_count; i++) {
        if (memcmp(guid, type_guid_map[i].guid, 16) == 0)
            return type_guid_map[i].name;
    }
    return "Unknown";
}

static void utf16_name_to_ascii(const uint16 *src, char *dst, uint32 dst_len) {
    if (dst_len == 0) return;
    //gpt names are utf16, we only keep basic ascii here
    uint32 i = 0;
    while (i + 1 < dst_len && src[i] != 0) {
        uint16 ch = src[i];
        dst[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        i++;
    }
    dst[i] = '\0';
}

static void format_bytes(uint64 bytes, char *buf, size bufsz) {
    //format size in whatever unit is least annoying to read
    if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
        uint64 whole = bytes / (1024ULL * 1024ULL * 1024ULL);
        uint64 frac = (bytes % (1024ULL * 1024ULL * 1024ULL)) / (1024ULL * 1024ULL * 100ULL);
        snprintf(buf, bufsz, "%lu.%02lu GiB", (unsigned long)whole, (unsigned long)frac);
    } else if (bytes >= (1024ULL * 1024ULL)) {
        uint64 whole = bytes / (1024ULL * 1024ULL);
        uint64 frac = (bytes % (1024ULL * 1024ULL)) / (1024ULL * 100ULL);
        snprintf(buf, bufsz, "%lu.%02lu MiB", (unsigned long)whole, (unsigned long)frac);
    } else if (bytes >= 1024ULL) {
        uint64 whole = bytes / 1024ULL;
        uint64 frac = (bytes % 1024ULL) / 10ULL;
        snprintf(buf, bufsz, "%lu.%02lu KiB", (unsigned long)whole, (unsigned long)frac);
    } else {
        snprintf(buf, bufsz, "%lu B", (unsigned long)bytes);
    }
}


static int read_lba(handle_t dev, uint32 sector_size, uint64 lba, void *buf) {
    uint64 offset = lba * (uint64)sector_size;
    if (handle_seek(dev, offset, HANDLE_SEEK_SET) < 0) return -1;
    int result = handle_read(dev, buf, sector_size);
    return result == (int)sector_size ? 0 : -1;
}

static void clear_partition_state(const char *status) {
    //wipe partition scan results but keep the selected disk
    partition_count = 0;
    selected_partition = 0;
    selected_is_gpt = false;
    strncpy(detail_status, status, sizeof(detail_status) - 1);
    detail_status[sizeof(detail_status) - 1] = '\0';
}

static int write_lba(handle_t dev, uint32 sector_size, uint64 lba, const void *buf) {
    uint64 offset = lba * (uint64)sector_size;
    int retries = 3;

    void *verify = malloc(sector_size);
    if (!verify) return -1; //cannot verify, treat as error

    while (retries--) {
        if (handle_seek(dev, offset, HANDLE_SEEK_SET) < 0) continue;
        if (handle_write(dev, buf, sector_size) != (int)sector_size) continue;

        //verify write
        if (handle_seek(dev, offset, HANDLE_SEEK_SET) < 0) continue;
        if (handle_read(dev, verify, sector_size) != (int)sector_size) continue;

        if (memcmp(buf, verify, sector_size) == 0) {
            free(verify);
            return 0; //success
        }
    }

    free(verify);
    return -2; //persistent corruption or failure
}

//write in-memory gpt_entry_t array back to disk (primary + backup headers)
//entry_buf must be a zeroed buffer large enough to hold all 128 entries
//returns 0 on success, -1 on error
static int commit_gpt(handle_t dev, uint32 sector_size,
                      gpt_header_t *primary_hdr, gpt_entry_t *entries,
                      uint32 num_entries) {
    //calculate partition entry array CRC32
    //we must calculate it over the full table as it will appear on disk
    uint32 full_table_size = num_entries * primary_hdr->partition_entry_size;
    uint8 *full_table = malloc(full_table_size);
    if (!full_table) return -1;
    memset(full_table, 0, full_table_size);

    //copy our cached entries into the correctly-sized table buffer
    for (uint32 i = 0; i < num_entries; i++) {
        memcpy(full_table + i * primary_hdr->partition_entry_size, &entries[i], sizeof(gpt_entry_t));
    }

    uint32 entries_crc = crc32(full_table, full_table_size);

    void *sector_buf = malloc(sector_size);
    if (!sector_buf) { free(full_table); return -1; }

    uint32 entries_per_sector = sector_size / primary_hdr->partition_entry_size;
    uint32 sector_count = (num_entries + entries_per_sector - 1) / entries_per_sector;

    //write primary entry array
    for (uint32 s = 0; s < sector_count; s++) {
        memset(sector_buf, 0, sector_size);
        uint32 first = s * entries_per_sector;
        uint32 count = entries_per_sector;
        if (first + count > num_entries) count = num_entries - first;

        if (first < num_entries) {
            memcpy(sector_buf, full_table + first * primary_hdr->partition_entry_size,
                   count * primary_hdr->partition_entry_size);
        }

        int res = write_lba(dev, sector_size, primary_hdr->partition_entry_lba + s, sector_buf);
        if (res < 0) {
            free(sector_buf); free(full_table);
            if (res == -2) clear_partition_state("disk write verification failed (primary table)");
            return -1;
        }
    }

    //write backup entry array
    uint64 backup_entry_lba = primary_hdr->alternate_lba - sector_count;
    for (uint32 s = 0; s < sector_count; s++) {
        memset(sector_buf, 0, sector_size);
        uint32 first = s * entries_per_sector;
        uint32 count = entries_per_sector;
        if (first + count > num_entries) count = num_entries - first;

        memcpy(sector_buf, full_table + first * primary_hdr->partition_entry_size,
               count * primary_hdr->partition_entry_size);

        int res = write_lba(dev, sector_size, backup_entry_lba + s, sector_buf);
        if (res < 0) {
            free(sector_buf); free(full_table);
            if (res == -2) clear_partition_state("disk write verification failed (backup table)");
            return -1;
        }
    }

    free(full_table);
    free(sector_buf);

    //update primary header checksums and write it
    primary_hdr->partition_entries_crc32 = entries_crc;
    primary_hdr->header_crc32 = 0;
    primary_hdr->header_crc32 = crc32(primary_hdr, primary_hdr->header_size);

    void *hdr_sector = malloc(sector_size);
    if (!hdr_sector) return -1;
    memset(hdr_sector, 0, sector_size);
    memcpy(hdr_sector, primary_hdr, sizeof(gpt_header_t));
    int res = write_lba(dev, sector_size, 1, hdr_sector);
    if (res < 0) {
        free(hdr_sector);
        if (res == -2) clear_partition_state("disk write verification failed (primary header)");
        return -1;
    }

    //build backup header (swap my_lba/alternate_lba and entry lba)
    gpt_header_t backup_hdr = *primary_hdr;
    backup_hdr.my_lba = primary_hdr->alternate_lba;
    backup_hdr.alternate_lba = 1;
    backup_hdr.partition_entry_lba = backup_entry_lba;
    backup_hdr.header_crc32 = 0;
    backup_hdr.header_crc32 = crc32(&backup_hdr, backup_hdr.header_size);

    memset(hdr_sector, 0, sector_size);
    memcpy(hdr_sector, &backup_hdr, sizeof(gpt_header_t));
    res = write_lba(dev, sector_size, primary_hdr->alternate_lba, hdr_sector);
    if (res < 0) {
        free(hdr_sector);
        if (res == -2) clear_partition_state("disk write verification failed (backup header)");
        return -1;
    }

    free(hdr_sector);
    return 0;
}

static uint32 get_rtc_time_seconds(void) {
    uint32 rtc_val = 0;
    handle_t sys_h = get_obj(INVALID_HANDLE, "$devices/system", RIGHT_GET_INFO);
    if (sys_h != INVALID_HANDLE) {
        time_stats_t ts;
        if (object_get_info(sys_h, OBJ_INFO_TIME_STATS, &ts, sizeof(ts)) == 0) {
            rtc_val = ts.rtc_time;
        }
        handle_close(sys_h);
    }
    return rtc_val;
}

static int initialize_gpt(handle_t dev, block_device_info_t *info) {
    if (info->sector_size < 512 || info->sector_count < 100) return -1;

    gpt_header_t hdr = {0};
    hdr.signature = GPT_SIGNATURE;
    hdr.revision = 0x00010000;
    hdr.header_size = 92;
    hdr.my_lba = 1;
    hdr.alternate_lba = info->sector_count - 1;

    //generate a random disk guid (using ticks and rtc time for better entropy)
    uint64 ticks = get_ticks();
    uint32 rtc_val = get_rtc_time_seconds();
    uint64 entropy1 = ticks ^ ((uint64)rtc_val << 32);
    uint64 entropy2 = ticks ^ rtc_val ^ 0x5555555555555555ULL;
    memcpy(hdr.disk_guid, &entropy1, 8);
    memcpy(hdr.disk_guid + 8, &entropy2, 8);
    hdr.disk_guid[6] = (hdr.disk_guid[6] & 0x0F) | 0x40; // UUID v4
    hdr.disk_guid[8] = (hdr.disk_guid[8] & 0x3F) | 0x80;

    hdr.partition_entry_lba = 2;
    hdr.num_partition_entries = 128;
    hdr.partition_entry_size = 128;

    uint64 entries_bytes = (uint64)hdr.num_partition_entries * hdr.partition_entry_size;
    uint64 entries_sectors = (entries_bytes + info->sector_size - 1) / info->sector_size;

    hdr.first_usable_lba = hdr.partition_entry_lba + entries_sectors;
    hdr.last_usable_lba = hdr.alternate_lba - (entries_sectors + 1);

    //create an empty table
    gpt_entry_t *empty_entries = calloc(128, sizeof(gpt_entry_t));
    if (!empty_entries) return -1;

    int res = commit_gpt(dev, info->sector_size, &hdr, empty_entries, 128);
    free(empty_entries);
    return res;
}

static int wipe_gpt(handle_t dev, block_device_info_t *info) {
    if (info->sector_size == 0) return -1;
    void *zero = malloc(info->sector_size);
    if (!zero) return -1;
    memset(zero, 0, info->sector_size);

    //zero the header and some entries
    for (uint64 lba = 0; lba < 64; lba++) {
        if (write_lba(dev, info->sector_size, lba, zero) < 0) {
            free(zero);
            clear_partition_state("disk wipe verification failed");
            return -1;
        }
    }
    //zero the backup at the end
    if (info->sector_count > 64) {
        for (uint64 lba = info->sector_count - 64; lba < info->sector_count; lba++) {
            if (write_lba(dev, info->sector_size, lba, zero) < 0) {
                free(zero);
                clear_partition_state("disk wipe verification failed (backup)");
                return -1;
            }
        }
    }

    free(zero);
    return 0;
}


static void inspect_selected_device(void) {
    //refresh partition info for the current disk
    if (device_count == 0 || selected_device >= device_count) {
        clear_partition_state("no disk selected");
        return;
    }

    device_entry_t *dev = &devices[selected_device];
    strncpy(detail_title, dev->name, sizeof(detail_title) - 1);
    detail_title[sizeof(detail_title) - 1] = '\0';

    handle_t h = get_obj(INVALID_HANDLE, dev->path, RIGHT_READ | RIGHT_GET_INFO);
    if (h == INVALID_HANDLE) {
        clear_partition_state("failed to open selected device");
        return;
    }

    uint32 sector_size = dev->info.sector_size ? dev->info.sector_size : 512;
    if (sector_size < sizeof(gpt_header_t) || sector_size > 4096) {
        handle_close(h);
        clear_partition_state("unsupported sector size");
        return;
    }

    void *sector = malloc(sector_size);
    if (!sector) {
        handle_close(h);
        clear_partition_state("out of memory");
        return;
    }

    if (read_lba(h, sector_size, 1, sector) < 0) {
        free(sector);
        handle_close(h);
        clear_partition_state("failed to read GPT header");
        return;
    }

    gpt_header_t *hdr = (gpt_header_t *)sector;
    if (hdr->signature != GPT_SIGNATURE) {
        free(sector);
        handle_close(h);
        clear_partition_state("device does not contain a GPT header");
        return;
    }

    selected_is_gpt = true;
    cached_gpt_hdr = *hdr;
    snprintf(detail_status, sizeof(detail_status),
             "gpt disk: usable LBA %lu..%lu",
             (unsigned long)hdr->first_usable_lba,
             (unsigned long)hdr->last_usable_lba);

    uint32 entry_size = hdr->partition_entry_size;
    if (entry_size < sizeof(gpt_entry_t) || entry_size > sector_size) {
        free(sector);
        handle_close(h);
        clear_partition_state("unsupported GPT entry size");
        return;
    }

    uint32 per_sector = sector_size / entry_size;
    uint32 max_entries = hdr->num_partition_entries;
    if (max_entries > 128) max_entries = 128;
    cached_num_entries = max_entries;

    //read all entries into cached_entries (raw, including empty ones)
    memset(cached_entries, 0, sizeof(cached_entries));
    for (uint32 i = 0; i < max_entries; i++) {
        uint64 lba = cached_gpt_hdr.partition_entry_lba + (i / per_sector);
        uint32 slot = i % per_sector;
        if (slot == 0) {
            if (read_lba(h, sector_size, lba, sector) < 0) break;
        }
        gpt_entry_t *src = (gpt_entry_t *)((uint8 *)sector + slot * entry_size);
        cached_entries[i] = *src;
    }

    //build display list from cached_entries
    partition_count = 0;
    for (uint32 i = 0; i < max_entries; i++) {
        gpt_entry_t *e = &cached_entries[i];
        if (guid_is_zero(e->type_guid)) continue;

        if (partition_count < MAX_PARTITIONS) {
            partition_info_t *out = &partitions[partition_count++];
            uint16 name_copy[36];
            memset(out, 0, sizeof(*out));
            memcpy(name_copy, e->name, sizeof(name_copy));
            out->valid = true;
            utf16_name_to_ascii(name_copy, out->name, sizeof(out->name));
            if (!out->name[0]) {
                snprintf(out->name, sizeof(out->name), "partition %lu", (unsigned long)partition_count);
            }
            strncpy(out->type_name, type_guid_name(e->type_guid), sizeof(out->type_name) - 1);
            guid_to_string(e->type_guid, out->type_guid, sizeof(out->type_guid));
            guid_to_string(e->partition_guid, out->guid, sizeof(out->guid));
            out->start_lba  = e->starting_lba;
            out->end_lba    = e->ending_lba;
            out->sectors    = out->end_lba - out->start_lba + 1;
            out->size_bytes = out->sectors * (uint64)sector_size;
            out->attributes = e->attributes;
            //store raw entry index so we can locate it in cached_entries later
            out->raw_index  = i;
        }
    }

    if (partition_count == 0) {
        snprintf(detail_status, sizeof(detail_status), "gpt disk has no populated entries");
    } else if (selected_partition >= partition_count) {
        selected_partition = partition_count - 1;
    }

    free(sector);
    handle_close(h);
}

static void do_delete_partition(void) {
    if (!selected_is_gpt || partition_count == 0) return;
    if (selected_partition >= partition_count) return;

    partition_info_t *p = &partitions[selected_partition];
    uint32 raw = p->raw_index;

    //open with write and get info rights
    device_entry_t *dev = &devices[selected_device];
    handle_t h = get_obj(INVALID_HANDLE, dev->path, RIGHT_READ | RIGHT_WRITE | RIGHT_GET_INFO);
    if (h == INVALID_HANDLE) {
        strncpy(detail_status, "error: cannot open device for writing", sizeof(detail_status) - 1);
        return;
    }

    uint32 sector_size = dev->info.sector_size ? dev->info.sector_size : 512;

    //zero the entry in the cache
    memset(&cached_entries[raw], 0, sizeof(gpt_entry_t));

    int err = commit_gpt(h, sector_size, &cached_gpt_hdr, cached_entries, cached_num_entries);
    if (err == 0) {
        //trigger kernel rescan so it unregisters old partition objects
        object_get_info(h, OBJ_INFO_BLOCK_RESCAN, NULL, 0);
    }
    handle_close(h);

    if (err < 0) {
        strncpy(detail_status, "error: failed to write GPT to disk", sizeof(detail_status) - 1);
        //restore entry from disk on failure
        inspect_selected_device();
        return;
    }

    //refresh display
    inspect_selected_device();
}

//line-input (used in dialogs)
//buf is pre-populated by the caller (used as the initial editable value)
//prompt is displayed at (col, row); result replaces buf on enter
//returns 0 on enter, -1 on escape
static int curses_readline(uint32 col, uint32 row, uint32 max_width,
                           char *buf, uint32 buflen) {
    if (buflen == 0) return -1;
    //seed length from whatever the caller already put in buf
    uint32 len = 0;
    while (len < buflen - 1 && buf[len]) len++;
    buf[len] = '\0';

    //show cursor immediately before the first paint
    curses_show_cursor(true);

    for (;;) {
        //draw field background then text
        curses_set_bg(CURSES_RGB(50, 50, 110));
        curses_set_fg(CURSES_RGB(255, 255, 160));
        curses_fill_rect(col, row, max_width, 1, ' ');
        curses_set_bg(CURSES_RGB(50, 50, 110));
        curses_set_fg(CURSES_RGB(255, 255, 160));
        curses_write_at(col, row, buf, max_width);
        curses_set_bg(CURSES_RGB(0, 0, 0));
        curses_set_cursor(col + len, row);
        curses_flush();

        kbd_event_t ev;
        if (curses_read(&ev) < 0) { curses_show_cursor(false); return -1; }
        if (!ev.pressed) continue;

        if (curses_event_is_submit(&ev)) {
            curses_show_cursor(false);
            return 0;
        }
        if (ev.codepoint == '\t') {
            curses_show_cursor(false);
            return 1;
        }
        if (ev.codepoint == 0x1B) { curses_show_cursor(false); return -1; }
        if (curses_event_is_backspace(&ev)) {
            if (len > 0) { len--; buf[len] = '\0'; }
            continue;
        }
        if (curses_event_is_printable(&ev) && len + 1 < buflen) {
            buf[len++] = curses_event_char(&ev);
            buf[len] = '\0';
        }
    }
}

//parse a decimal uint64 string, returns -1 on error
static int parse_u64(const char *s, uint64 *out) {
    if (!s || !s[0]) return -1;
    uint64 v = 0;
    for (uint32 i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        uint64 digit = (uint64)(s[i] - '0');
        if (v > (UINT64_MAX - digit) / 10) return -1;
        v = v * 10 + digit;
    }
    *out = v;
    return 0;
}

//draw one labeled row in the new-partition dialog
//active=true highlights it as the field being edited
static void np_draw_field(uint32 bx, uint32 by, uint32 bw,
                          uint32 field_row, const char *label, uint32 label_col,
                          const char *value, uint32 value_col, uint32 value_w,
                          bool active) {
    //label
    curses_set_bg(CURSES_RGB(20, 20, 40));
    curses_set_fg(active ? CURSES_RGB(255, 255, 255) : CURSES_RGB(160, 160, 160));
    curses_write_at(bx + label_col, by + field_row, label, bw - label_col - 1);

    //value box: bright bg when active, dark when idle
    if (active) {
        curses_set_bg(CURSES_RGB(50, 50, 110));
        curses_set_fg(CURSES_RGB(255, 255, 160));
    } else {
        curses_set_bg(CURSES_RGB(28, 28, 55));
        curses_set_fg(CURSES_RGB(200, 200, 140));
    }
    curses_fill_rect(bx + value_col, by + field_row, value_w, 1, ' ');
    curses_write_at(bx + value_col, by + field_row, value, value_w);
    curses_set_bg(CURSES_RGB(20, 20, 40));
}

static bool do_new_partition(void) {
    if (!selected_is_gpt) return false;

    //find first empty slot (type GUID is null)
    int slot = -1;
    for (int i = 0; i < (int)cached_num_entries; i++) {
        if (guid_is_zero(cached_entries[i].type_guid)) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        strncpy(detail_status, "error: GPT table is full", sizeof(detail_status) - 1);
        return false;
    }

    uint32 scr_rows = curses_get_rows();
    uint32 scr_cols = curses_get_cols();
    uint32 bw = 62, bh = 16;
    uint32 bx = (scr_cols > bw + 4) ? (scr_cols - bw) / 2 : 2;
    uint32 by = (scr_rows > bh + 4) ? (scr_rows - bh) / 2 : 2;

    uint32 sector_size = devices[selected_device].info.sector_size
                         ? devices[selected_device].info.sector_size : 512;

    //editable field buffers with defaults pre-filled
    char start_buf[24], size_buf[24], name_buf[40];
    uint64 start_lba = cached_gpt_hdr.first_usable_lba;
    uint64 default_sectors = (1ULL * 1024 * 1024 + sector_size - 1) / sector_size;
    bool found_overlap;
    do {
        found_overlap = false;
        for (uint32 i = 0; i < partition_count; i++) {
            uint64 p_start = partitions[i].start_lba;
            uint64 p_end = partitions[i].end_lba;
            if (start_lba <= p_end && p_start <= start_lba + default_sectors - 1) {
                start_lba = p_end + 1;
                found_overlap = true;
                break;
            }
        }
    } while (found_overlap);

    if (start_lba + default_sectors - 1 > cached_gpt_hdr.last_usable_lba) {
        //if above failed then fallback, find  the first free sector of any size
        start_lba = cached_gpt_hdr.first_usable_lba;
        do {
            found_overlap = false;
            for (uint32 i = 0; i < partition_count; i++) {
                if (start_lba >= partitions[i].start_lba && start_lba <= partitions[i].end_lba) {
                    start_lba = partitions[i].end_lba + 1;
                    found_overlap = true;
                    break;
                }
            }
        } while (found_overlap);

        if (start_lba > cached_gpt_hdr.last_usable_lba) {
            start_lba = cached_gpt_hdr.first_usable_lba;
        }
    }
    uint64 size_mib  = 1;
    uint32 type_sel  = 0;

    snprintf(start_buf, sizeof(start_buf), "%lu", (unsigned long)start_lba);
    snprintf(size_buf,  sizeof(size_buf),  "%lu", (unsigned long)size_mib);
    name_buf[0] = '\0';

    //field layout constants (column offsets within box)
    const uint32 LABEL_COL  = 2;
    const uint32 VALUE_COL  = 20;
    const uint32 VALUE_W    = bw - VALUE_COL - 3;
    //rows within box for each field
    const uint32 field_rows[NP_FIELD_COUNT] = {3, 5, 7, 9};
    const char  *labels[NP_FIELD_COUNT]     = {
        "Start LBA:", "Size (MiB):", "Type:", "Name:"
    };

    uint32 active_field = NP_FIELD_START;
    bool done_ok = false;

    for (;;) {
        //draw the dialog frame
        curses_set_bg(CURSES_RGB(20, 20, 40));
        curses_set_fg(CURSES_RGB(100, 180, 255));
        curses_fill_rect(bx, by, bw, bh, ' ');
        curses_draw_box(bx, by, bw, bh);
        curses_write_at(bx + 2, by, " New Partition ", bw - 4);

        //hint line
        curses_set_bg(CURSES_RGB(20, 20, 40));
        curses_set_fg(CURSES_RGB(120, 120, 120));
        curses_write_at(bx + 2, by + bh - 3,
                        "Tab=next field/cycle   t=cycle type   Esc=cancel", bw - 4);
        curses_write_at(bx + 2, by + bh - 2,
                        "Enter on last field confirms.", bw - 4);

        //draw the four fields
        //type field value string
        char type_val[32];
        snprintf(type_val, sizeof(type_val), "%s", type_guid_map[type_sel].name);

        np_draw_field(bx, by, bw, field_rows[NP_FIELD_START], labels[NP_FIELD_START],
                      LABEL_COL, start_buf, VALUE_COL, VALUE_W,
                      active_field == NP_FIELD_START);
        np_draw_field(bx, by, bw, field_rows[NP_FIELD_SIZE], labels[NP_FIELD_SIZE],
                      LABEL_COL, size_buf, VALUE_COL, VALUE_W,
                      active_field == NP_FIELD_SIZE);
        np_draw_field(bx, by, bw, field_rows[NP_FIELD_TYPE], labels[NP_FIELD_TYPE],
                      LABEL_COL, type_val, VALUE_COL, VALUE_W,
                      active_field == NP_FIELD_TYPE);
        np_draw_field(bx, by, bw, field_rows[NP_FIELD_NAME], labels[NP_FIELD_NAME],
                      LABEL_COL, name_buf, VALUE_COL, VALUE_W,
                      active_field == NP_FIELD_NAME);

        if (active_field == NP_FIELD_TYPE) {
            curses_set_cursor(bx + VALUE_COL, by + field_rows[NP_FIELD_TYPE]);
            curses_show_cursor(true);
        } else {
            curses_show_cursor(false);
        }
        curses_set_bg(CURSES_RGB(0, 0, 0));
        curses_flush();

        //handle the active field
        if (active_field == NP_FIELD_TYPE) {
            kbd_event_t ev;
            if (curses_read(&ev) < 0) goto cancel;
            if (!ev.pressed) continue;
            if (ev.codepoint == 0x1B) { curses_show_cursor(false); goto cancel; }
            if (curses_event_is_submit(&ev) || ev.codepoint == '\t') {
                curses_show_cursor(false);
                //advance to next field
                active_field = (active_field + 1) % NP_FIELD_COUNT;
                continue;
            }
            if (curses_event_is_printable(&ev)) {
                char c = curses_event_char(&ev);
                if (c == 't' || c == 'T')
                    type_sel = (type_sel + 1) % type_guid_count;
            }
            continue;
        }

        //text input fields
        char *active_buf;
        uint32 active_bufsz;
        switch (active_field) {
            case NP_FIELD_START: active_buf = start_buf; active_bufsz = sizeof(start_buf); break;
            case NP_FIELD_SIZE:  active_buf = size_buf;  active_bufsz = sizeof(size_buf);  break;
            default:             active_buf = name_buf;  active_bufsz = sizeof(name_buf);  break;
        }

        uint32 vx = bx + VALUE_COL;
        uint32 vy = by + field_rows[active_field];
        int rc = curses_readline(vx, vy, VALUE_W, active_buf, active_bufsz);
        if (rc < 0) goto cancel;

        bool is_tab = (rc == 1);

        //validate and advance
        if (active_field == NP_FIELD_START) {
            uint64 tmp;
            if (parse_u64(start_buf, &tmp) < 0
                    || tmp < cached_gpt_hdr.first_usable_lba
                    || tmp > cached_gpt_hdr.last_usable_lba) {
                strncpy(detail_status, "invalid start LBA", sizeof(detail_status) - 1);
                //restore default so field is valid again
                start_lba = cached_gpt_hdr.first_usable_lba;
                snprintf(start_buf, sizeof(start_buf), "%lu", (unsigned long)start_lba);
            } else {
                start_lba = tmp;
            }
            active_field = NP_FIELD_SIZE;

        } else if (active_field == NP_FIELD_SIZE) {
            uint64 tmp;
            if (parse_u64(size_buf, &tmp) < 0 || tmp == 0) {
                strncpy(detail_status, "invalid size", sizeof(detail_status) - 1);
                size_mib = 1;
                snprintf(size_buf, sizeof(size_buf), "%lu", (unsigned long)size_mib);
            } else {
                size_mib = tmp;
            }
            active_field = NP_FIELD_TYPE;

        } else if (active_field == NP_FIELD_NAME) {
            if (is_tab) {
                active_field = NP_FIELD_START;
            } else {
                //name field - last one confirm
                done_ok = true;
                break;
            }
        }
        continue;

    cancel:
        done_ok = false;
        break;
    }

    if (!done_ok) return false;

    //guard size-to-LBA overflow
    if (size_mib > UINT64_MAX / (1024ULL * 1024ULL)) {
        strncpy(detail_status, "error: partition size is too large (overflow)", sizeof(detail_status) - 1);
        return false;
    }

    //compute LBA range
    uint64 sectors_needed = (size_mib * 1024ULL * 1024ULL + sector_size - 1) / sector_size;
    if (sectors_needed == 0) {
        strncpy(detail_status, "error: partition size results in 0 sectors", sizeof(detail_status) - 1);
        return false;
    }

    if (start_lba > UINT64_MAX - (sectors_needed - 1)) {
        strncpy(detail_status, "error: partition address overflow", sizeof(detail_status) - 1);
        return false;
    }

    uint64 end_lba = start_lba + sectors_needed - 1;
    if (end_lba > cached_gpt_hdr.last_usable_lba) {
        strncpy(detail_status, "error: partition extends beyond usable space", sizeof(detail_status) - 1);
        return false;
    }

    //check for range overlaps with existing partitions
    for (int i = 0; i < (int)cached_num_entries; i++) {
        gpt_entry_t *entry = &cached_entries[i];
        if (guid_is_zero(entry->type_guid) || entry->starting_lba == 0 || entry->ending_lba == 0) {
            continue;
        }
        if (start_lba <= entry->ending_lba && entry->starting_lba <= end_lba) {
            strncpy(detail_status, "error: partition overlaps with existing partition", sizeof(detail_status) - 1);
            return false;
        }
    }

    //fill the entry
    gpt_entry_t *e = &cached_entries[slot];
    memset(e, 0, sizeof(*e));
    memcpy(e->type_guid, type_guid_map[type_sel].guid, 16);

    //generate a unique partition GUID using get_ticks(), RTC time, slot, and a local counter
    static uint32 pguid_counter = 0;
    pguid_counter++;
    uint64 ticks = get_ticks();
    uint32 rtc_val = get_rtc_time_seconds();
    uint32 val1 = (uint32)(ticks & 0xFFFFFFFF) ^ rtc_val;
    uint32 val2 = (uint32)((ticks >> 32) & 0xFFFFFFFF) ^ (rtc_val >> 8);
    uint32 val3 = (uint32)(start_lba & 0xFFFFFFFF) ^ (rtc_val >> 16);
    uint32 val4 = ((pguid_counter << 16) | (uint16)slot) ^ (rtc_val >> 24);
    memcpy(&e->partition_guid[0], &val1, 4);
    memcpy(&e->partition_guid[4], &val2, 4);
    memcpy(&e->partition_guid[8], &val3, 4);
    memcpy(&e->partition_guid[12], &val4, 4);
    e->partition_guid[6] = (e->partition_guid[6] & 0x0F) | 0x40; // UUID v4
    e->partition_guid[8] = (e->partition_guid[8] & 0x3F) | 0x80;

    e->starting_lba = start_lba;
    e->ending_lba   = end_lba;
    e->attributes   = 0;
    for (uint32 k = 0; k < 36 && name_buf[k]; k++)
        e->name[k] = (uint16)(unsigned char)name_buf[k];

    //commit to disk
    device_entry_t *dev = &devices[selected_device];
    handle_t h = get_obj(INVALID_HANDLE, dev->path, RIGHT_READ | RIGHT_WRITE | RIGHT_GET_INFO);
    if (h == INVALID_HANDLE) {
        strncpy(detail_status, "error: cannot open device for writing", sizeof(detail_status) - 1);
        memset(e, 0, sizeof(*e));
        return false;
    }
    int err = commit_gpt(h, sector_size, &cached_gpt_hdr, cached_entries, cached_num_entries);
    if (err == 0) {
        object_get_info(h, OBJ_INFO_BLOCK_RESCAN, NULL, 0);
    }
    handle_close(h);
    if (err < 0) {
        strncpy(detail_status, "error: failed to write GPT", sizeof(detail_status) - 1);
        memset(e, 0, sizeof(*e));
        return false;
    }
    inspect_selected_device();
    return true;
}

//rescan $devices/disks and rebuild the left pane
static void reload_devices(void) {
    device_count = 0;
    partition_count = 0;
    selected_is_gpt = false;
    detail_title[0] = '\0';
    detail_status[0] = '\0';

    handle_t dir = get_obj(INVALID_HANDLE, "$devices/disks", RIGHT_READ);
    if (dir == INVALID_HANDLE) {
        clear_partition_state("failed to open $devices/disks");
        return;
    }

    dirent_t entries[16];
    uint32 index = 0;
    while (device_count < MAX_DEVICES) {
        //stateless readdir, index is updated by the call
        int n = readdir(dir, entries, 16, &index);
        if (n <= 0) break;

        for (int i = 0; i < n && device_count < MAX_DEVICES; i++) {
            device_entry_t *out = &devices[device_count];
            memset(out, 0, sizeof(*out));
            strncpy(out->name, entries[i].name, sizeof(out->name) - 1);
            snprintf(out->path, sizeof(out->path), "$devices/disks/%s", entries[i].name);

            handle_t h = get_obj(INVALID_HANDLE, out->path, RIGHT_GET_INFO);
            if (h == INVALID_HANDLE) continue;

            //skip partitions (names ending in p[0-9]+ like nvme1n1p1)
            char *p = strrchr(out->name, 'p');
            if (p && p > out->name && p[1] != '\0') {
                bool all_digits = true;
                for (int j = 1; p[j] != '\0'; j++) {
                    if (p[j] < '0' || p[j] > '9') {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits) {
                    handle_close(h);
                    continue;
                }
            }

            if (object_get_info(h, OBJ_INFO_BLOCK_DEVICE, &out->info, sizeof(out->info)) == 0) {
                //only keep entries that actually expose block device info
                device_count++;
            }
            handle_close(h);
        }
    }

    handle_close(dir);

    if (device_count == 0) {
        clear_partition_state("no block devices found");
        return;
    }

    if (selected_device >= device_count) selected_device = device_count - 1;
    inspect_selected_device();
    if (partition_count == 0) focus_pane = FOCUS_DEVICES;
}

static void draw_devices_pane(uint32 x, uint32 y, uint32 w, uint32 h) {
    //left pane with disks
    curses_set_fg(focus_pane == FOCUS_DEVICES ? CURSES_RGB(120, 220, 255) : CURSES_RGB(80, 140, 170));
    curses_draw_box(x, y, w, h);
    curses_write_at(x + 2, y, " disks ", w - 4);

    if (h <= 2) return;

    uint32 inner_y = y + 1;
    uint32 visible = h - 2;
    uint32 start = 0;
    if (selected_device >= visible) start = selected_device - visible + 1;

    for (uint32 i = 0; i < visible && start + i < device_count; i++) {
        uint32 idx = start + i;
        char size_buf[24];
        char line[96];
        format_bytes(devices[idx].info.sector_count * (uint64)devices[idx].info.sector_size, size_buf, sizeof(size_buf));

        snprintf(line, sizeof(line), "%c %s", idx == selected_device ? '>' : ' ', devices[idx].name);
        if (idx == selected_device) {
            curses_set_fg(CURSES_RGB(255, 240, 170));
        } else {
            curses_set_fg(CURSES_RGB(220, 220, 220));
        }
        curses_write_at(x + 2, inner_y + i, line, w > 4 ? w - 4 : 0);
        curses_write_at(x + 2 + strlen(line) + 1, inner_y + i, size_buf,
                        w > 4 + strlen(line) + 1 ? w - 4 - (uint32)strlen(line) - 1 : 0);
    }
}

static void draw_details_pane(uint32 x, uint32 y, uint32 w, uint32 h) {
    //right pane with disk and partition details
    curses_set_fg(CURSES_RGB(255, 200, 120));
    curses_draw_box(x, y, w, h);
    curses_write_at(x + 2, y, " details ", w - 4);

    if (device_count == 0 || selected_device >= device_count || h <= 2) return;

    device_entry_t *dev = &devices[selected_device];
    char buf[128];
    char size_buf[24];
    format_bytes(dev->info.sector_count * (uint64)dev->info.sector_size, size_buf, sizeof(size_buf));

    curses_set_fg(CURSES_RGB(230, 230, 230));
    snprintf(buf, sizeof(buf), "device: %s", dev->name);
    curses_write_at(x + 2, y + 2, buf, w - 4);
    snprintf(buf, sizeof(buf), "path: %s", dev->path);
    curses_write_at(x + 2, y + 3, buf, w - 4);
    snprintf(buf, sizeof(buf), "size: %s", size_buf);
    curses_write_at(x + 2, y + 4, buf, w - 4);
    snprintf(buf, sizeof(buf), "sector: %lu bytes", (unsigned long)dev->info.sector_size);
    curses_write_at(x + 2, y + 5, buf, w - 4);
    snprintf(buf, sizeof(buf), "status: %s", detail_status);
    curses_write_at(x + 2, y + 6, buf, w - 4);

    if (!selected_is_gpt || partition_count == 0) return;

    //partition list only appears for gpt disks
    curses_set_fg(focus_pane == FOCUS_PARTITIONS ? CURSES_RGB(180, 255, 180) : CURSES_RGB(120, 170, 120));
    curses_write_at(x + 2, y + 8, "partitions", w - 4);

    uint32 list_h = (h > 17) ? 8 : (h > 13 ? h - 9 : 3);
    uint32 visible = list_h / 2;
    if (visible == 0) visible = 1;
    uint32 start = 0;
    if (selected_partition >= visible) start = selected_partition - visible + 1;

    uint32 line_y = y + 9;
    for (uint32 i = 0; i < visible && start + i < partition_count && line_y + i * 2 + 1 < y + h - 1; i++) {
        uint32 idx = start + i;
        partition_info_t *p = &partitions[idx];
        char psize[24];
        format_bytes(p->size_bytes, psize, sizeof(psize));

        if (idx == selected_partition) {
            curses_set_fg(CURSES_RGB(255, 240, 170));
        } else {
            curses_set_fg(CURSES_RGB(255, 255, 255));
        }
        snprintf(buf, sizeof(buf), "%c %lu. %s (slot %u)", idx == selected_partition ? '>' : ' ',
                 (unsigned long)(idx + 1), p->name, p->raw_index);
        curses_write_at(x + 2, line_y + i * 2, buf, w - 4);

        curses_set_fg(CURSES_RGB(170, 170, 170));
        snprintf(buf, sizeof(buf), "%s  %s", p->type_name, psize);
        curses_write_at(x + 4, line_y + i * 2 + 1, buf, w - 6);
    }

    partition_info_t *sel = &partitions[selected_partition];
    //selected partition details at the bottom
    uint32 info_y = line_y + visible * 2 + 1;
    if (info_y + 6 >= y + h) return;

    curses_set_fg(CURSES_RGB(255, 220, 180));
    curses_write_at(x + 2, info_y, "selected partition", w - 4);
    curses_set_fg(CURSES_RGB(230, 230, 230));
    snprintf(buf, sizeof(buf), "name: %s", sel->name);
    curses_write_at(x + 2, info_y + 1, buf, w - 4);
    snprintf(buf, sizeof(buf), "type: %s", sel->type_name);
    curses_write_at(x + 2, info_y + 2, buf, w - 4);
    snprintf(buf, sizeof(buf), "range: LBA %lu..%lu", (unsigned long)sel->start_lba, (unsigned long)sel->end_lba);
    curses_write_at(x + 2, info_y + 3, buf, w - 4);
    snprintf(buf, sizeof(buf), "guid: %s", sel->guid);
    curses_write_at(x + 2, info_y + 4, buf, w - 4);
    snprintf(buf, sizeof(buf), "type guid: %s", sel->type_guid);
    curses_write_at(x + 2, info_y + 5, buf, w - 4);
    snprintf(buf, sizeof(buf), "attrs: 0x%lx", (unsigned long)sel->attributes);
    curses_write_at(x + 2, info_y + 6, buf, w - 4);
}

static void redraw(void) {
    //full ui redraw from the cached state
    curses_invalidate();
    curses_reset_style();
    curses_set_bg(CURSES_RGB(0, 0, 0));
    curses_set_fg(CURSES_RGB(235, 235, 235));
    curses_clear();
    curses_show_cursor(false);

    uint32 rows = curses_get_rows();
    uint32 cols = curses_get_cols();
    if (rows < 8 || cols < 40) {
        //tiny terminals get a fallback message
        curses_center_text(0, "partman");
        curses_write_at(2, 2, "terminal too small for partition manager", cols > 4 ? cols - 4 : 0);
        curses_flush();
        return;
    }

    curses_set_fg(CURSES_RGB(120, 220, 255));
    curses_center_text(0, "DeltaOS Partition Manager");
    curses_set_fg(CURSES_RGB(180, 180, 180));
    curses_center_text(1, "GPT partition editor - i:init  n:new  d:delete  W:wipe  r:reload  q:quit");

    uint32 left_w = cols / 3;
    if (left_w < 20) left_w = 20;
    if (left_w > cols - 22) left_w = cols - 22;
    uint32 body_y = 3;
    uint32 body_h = rows - 6;

    //layout is left list, right details, bottom help box
    draw_devices_pane(2, body_y, left_w, body_h);
    draw_details_pane(left_w + 3, body_y, cols - left_w - 5, body_h);

    curses_set_fg(CURSES_RGB(140, 140, 140));
    curses_draw_box(2, rows - 3, cols - 4, 3);
    curses_write_at(4, rows - 3, " help ", cols - 8);
    curses_write_at(4, rows - 2, "tab switch pane   j/k move   i init   n new   d delete   W wipe   r reload   q quit", cols - 8);
    curses_flush();
}

int main(void) {
    if (curses_init() < 0) {
        puts("partman: failed to initialize curses\n");
        return 1;
    }

    //initial scan and first paint
    reload_devices();
    redraw();

    while (1) {
        kbd_event_t ev;
        if (curses_read(&ev) < 0) break;

        if (!ev.pressed) continue;

        if (ev.codepoint == '\t') {
            //tab switches focus only when partitions exist
            if (partition_count > 0) {
                focus_pane = (focus_pane == FOCUS_DEVICES) ? FOCUS_PARTITIONS : FOCUS_DEVICES;
                redraw();
            }
            continue;
        }

        if (!curses_event_is_printable(&ev)) continue;
        char ch = curses_event_char(&ev);

        //q quits, r rescans, j/k moves selection, n/d modify partitions
        if (ch == 'q' || ch == 'Q') break;
        if (ch == 'r' || ch == 'R') {
            reload_devices();
            redraw();
            continue;
        }
        if (ch == 'W') { //shift+W to wipe
            if (device_count > 0) {
                handle_t h = get_obj(INVALID_HANDLE, devices[selected_device].path, RIGHT_READ | RIGHT_WRITE | RIGHT_GET_INFO);
                if (h != INVALID_HANDLE) {
                    wipe_gpt(h, &devices[selected_device].info);
                    object_get_info(h, OBJ_INFO_BLOCK_RESCAN, NULL, 0);
                    handle_close(h);
                    reload_devices();
                }
            }
            redraw();
            continue;
        }
        if (ch == 'i' || ch == 'I') { //initialize GPT
            if (device_count > 0) {
                handle_t h = get_obj(INVALID_HANDLE, devices[selected_device].path, RIGHT_READ | RIGHT_WRITE | RIGHT_GET_INFO);
                if (h != INVALID_HANDLE) {
                    initialize_gpt(h, &devices[selected_device].info);
                    object_get_info(h, OBJ_INFO_BLOCK_RESCAN, NULL, 0);
                    handle_close(h);
                    reload_devices();
                }
            }
            redraw();
            continue;
        }
        if ((ch == 'n' || ch == 'N') && selected_is_gpt) {
            if (do_new_partition()) {
                reload_devices();
            }
            redraw();
            continue;
        }
        //delete partition - only from partition pane
        if ((ch == 'd' || ch == 'D') && focus_pane == FOCUS_PARTITIONS && partition_count > 0) {
            //confirm before nuking
            uint32 rows = curses_get_rows();
            uint32 cols = curses_get_cols();
            uint32 bw = 46, bh = 5;
            uint32 bx = (cols > bw + 4) ? (cols - bw) / 2 : 2;
            uint32 by = (rows > bh + 4) ? (rows - bh) / 2 : 2;
            curses_set_bg(CURSES_RGB(40, 10, 10));
            curses_set_fg(CURSES_RGB(255, 80, 80));
            curses_fill_rect(bx, by, bw, bh, ' ');
            curses_draw_box(bx, by, bw, bh);
            curses_write_at(bx + 2, by,     " Confirm Delete ", bw - 4);
            curses_set_fg(CURSES_RGB(230, 230, 230));
            curses_write_at(bx + 2, by + 2, "Delete selected partition? y=yes  n=no", bw - 4);
            curses_flush();
            kbd_event_t cev;
            bool confirmed = false;
            while (curses_read(&cev) == 0) {
                if (!cev.pressed) continue;
                if (!curses_event_is_printable(&cev)) continue;
                char cc = curses_event_char(&cev);
                if (cc == 'y' || cc == 'Y') { confirmed = true; break; }
                break;
            }
            if (confirmed) {
                do_delete_partition();
                reload_devices();
            }
            redraw();
            continue;
        }
        if ((ch == 'j' || ch == 'J') && focus_pane == FOCUS_DEVICES && selected_device + 1 < device_count) {
            selected_device++;
            selected_partition = 0;
            inspect_selected_device();
            redraw();
            continue;
        }
        if ((ch == 'k' || ch == 'K') && focus_pane == FOCUS_DEVICES && selected_device > 0) {
            selected_device--;
            selected_partition = 0;
            inspect_selected_device();
            redraw();
            continue;
        }
        if ((ch == 'j' || ch == 'J') && focus_pane == FOCUS_PARTITIONS && selected_partition + 1 < partition_count) {
            selected_partition++;
            redraw();
            continue;
        }
        if ((ch == 'k' || ch == 'K') && focus_pane == FOCUS_PARTITIONS && selected_partition > 0) {
            selected_partition--;
            redraw();
            continue;
        }

    }

    curses_show_cursor(true);
    curses_shutdown();
    return 0;
}
