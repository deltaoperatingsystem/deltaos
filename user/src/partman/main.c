#include <curses.h>
#include <io.h>
#include <mem.h>
#include <string.h>
#include <system.h>

#define MAX_DEVICES 32
#define MAX_PARTITIONS 32
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
    static const uint8 esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    static const uint8 basic_guid[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    static const uint8 linux_guid[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };

    if (memcmp(guid, esp_guid, 16) == 0) return "EFI system";
    if (memcmp(guid, basic_guid, 16) == 0) return "Basic data";
    if (memcmp(guid, linux_guid, 16) == 0) return "Linux fs";
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
    //seek once then read one logical sector
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

    //we only care about gpt right now
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
    //show usable lba range in the details pane
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

    //cap the scan so the ui does not explode on weird tables
    uint32 per_sector = sector_size / entry_size;
    uint32 max_entries = hdr->num_partition_entries;
    if (max_entries > 128) max_entries = 128;

    partition_count = 0;
    for (uint32 i = 0; i < max_entries && partition_count < MAX_PARTITIONS; i++) {
        //gpt entries are packed into a flat array across sectors
        uint64 lba = hdr->partition_entry_lba + (i / per_sector);
        uint32 slot = i % per_sector;

        if (slot == 0) {
            if (read_lba(h, sector_size, lba, sector) < 0) {
                snprintf(detail_status, sizeof(detail_status), "failed reading GPT entry LBA %lu", (unsigned long)lba);
                break;
            }
        }

        gpt_entry_t *entry = (gpt_entry_t *)((uint8 *)sector + slot * entry_size);
        if (guid_is_zero(entry->type_guid)) continue;

        partition_info_t *out = &partitions[partition_count++];
        //convert one entry into displayable strings
        uint16 name_copy[36];
        memset(out, 0, sizeof(*out));
        memcpy(name_copy, entry->name, sizeof(name_copy));
        out->valid = true;
        utf16_name_to_ascii(name_copy, out->name, sizeof(out->name));
        if (!out->name[0]) {
            snprintf(out->name, sizeof(out->name), "partition %lu", (unsigned long)partition_count);
        }
        strncpy(out->type_name, type_guid_name(entry->type_guid), sizeof(out->type_name) - 1);
        guid_to_string(entry->type_guid, out->type_guid, sizeof(out->type_guid));
        guid_to_string(entry->partition_guid, out->guid, sizeof(out->guid));
        out->start_lba = entry->starting_lba;
        out->end_lba = entry->ending_lba;
        out->sectors = out->end_lba - out->start_lba + 1;
        out->size_bytes = out->sectors * (uint64)sector_size;
        out->attributes = entry->attributes;
    }

    if (partition_count == 0) {
        snprintf(detail_status, sizeof(detail_status), "gpt disk has no populated entries");
    } else if (selected_partition >= partition_count) {
        selected_partition = partition_count - 1;
    }

    free(sector);
    handle_close(h);
}

static void reload_devices(void) {
    //rescan $devices/disks and rebuild the left pane
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
        snprintf(buf, sizeof(buf), "%c %lu. %s", idx == selected_partition ? '>' : ' ',
                 (unsigned long)(idx + 1), p->name);
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
    curses_center_text(1, "read-only view for disks and GPT partitions");

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
    curses_write_at(4, rows - 2, "tab switch pane   j/k move   r reload   q quit", cols - 8);
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

        //q quits, r rescans, j/k moves selection
        if (ch == 'q' || ch == 'Q') break;
        if (ch == 'r' || ch == 'R') {
            reload_devices();
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
