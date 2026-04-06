#include <fs/fat32.h>
#include <fs/mount.h>
#include <drivers/blkdev.h>
#include <mm/kheap.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/spinlock.h>
#include <syscall/syscall.h>

#define FAT32_MAX_NAME 12
#define FAT32_MAX_PATH 256

#define FAT32_ATTR_READONLY 0x01
#define FAT32_ATTR_HIDDEN   0x02
#define FAT32_ATTR_SYSTEM   0x04
#define FAT32_ATTR_VOLUME   0x08
#define FAT32_ATTR_DIR      0x10
#define FAT32_ATTR_ARCHIVE  0x20
#define FAT32_ATTR_LFN      0x0F
#define FAT32_LFN_MAX_ENTRIES 20

#define FAT32_EOC_MIN       0x0FFFFFF8
#define FAT32_EOC_MASK      0x0FFFFFFF

typedef struct __attribute__((packed)) {
    uint8 jump[3];
    //boot sector oem string
    char oem[8];
    uint16 bytes_per_sector;
    uint8 sectors_per_cluster;
    uint16 reserved_sectors;
    uint8 fat_count;
    uint16 root_entry_count;   //fat16 only
    uint16 total_sectors_16;   //fat16 only
    uint8 media;               //usually f8 on hard disks
    uint16 fat_size_16;        //fat16 only
    uint16 sectors_per_track;
    uint16 heads;
    uint32 hidden_sectors;     //partition start lba
    uint32 total_sectors_32;
    uint32 fat_size_32;        //size of one fat in sectors
    uint16 ext_flags;          //active fat and mirroring flags
    uint16 fs_version;         //should be 0
    uint32 root_cluster;       //root dir start cluster
    uint16 fsinfo_sector;      //fsinfo sector
    uint16 backup_boot_sector;
    uint8 reserved[12];        //bios junk and padding
    uint8 drive_number;        //int 13h drive number
    uint8 reserved1;
    uint8 boot_sig;            //29 means the next fields are valid
    uint32 volume_id;          //volume serial
    //11 byte label, space padded, no terminator
    char volume_label[11];
    //usually FAT32
    char fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    //8.3 short name, no dot abd no terminator
    char name[11];
    uint8 attr;
    uint8 ntres;
    uint8 crt_time_tenth;      //creation time fine bits
    uint16 crt_time;           //hh mm ss/2
    uint16 crt_date;           //yyyy mm dd
    uint16 access_date;        //last access date
    uint16 first_cluster_hi;   //high half of first cluster
    uint16 wrt_time;           //last write time
    uint16 wrt_date;           //last write date
    uint16 first_cluster_lo;   //low half of first cluster
    uint32 size;               //file size in bytes
} fat32_dirent_t;

typedef struct __attribute__((packed)) {
    uint8 order;
    uint16 name1[5];           //chars 1 to 5
    uint8 attr;
    uint8 type;                //always 0
    uint8 checksum;            //short name checksum
    uint16 name2[6];           //chars 6 to 11
    uint16 first_cluster_lo;   //always 0
    uint16 name3[2];           //chars 12 to 13
} fat32_lfn_dirent_t;

typedef struct fat32_fs fat32_fs_t;

typedef struct fat32_node {
    fat32_fs_t *fs;
    uint32 first_cluster;      //first data cluster
    uint32 size;               //file size
    uint32 attr;               //cached attr field
    uint32 parent_cluster;     //parent dir cluster
    uint64 dir_entry_offset;   //short entry offset in parent
    bool is_dir;
    bool is_root;              //root has no dirent
} fat32_node_t;

struct fat32_fs {
    object_t *source;          //backing block device
    uint32 dev_sector_size;    //device sector size
    uint64 dev_sector_count;

    uint32 bytes_per_sector;   //from BPB
    uint32 sectors_per_cluster; //from BPB
    uint32 reserved_sectors;    //from BPB
    uint32 fat_count;           //from BPB
    uint32 fat_size_sectors;    //from BPB
    uint32 root_cluster;        //from BPB
    uint32 fsinfo_sector;       //from BPB
    uint32 total_sectors;       //from BPB
    uint32 total_clusters;      //derived

    uint64 fat_offset;          //FAT start in bytes
    uint64 data_offset;         //cluster 2 start in bytes
    uint64 cluster_size;        //bytes per cluster

    uint32 next_free_cluster;   //roving allocation hint
    spinlock_t lock;
    fs_t *fs;
};

static ssize fat32_file_read(object_t *obj, void *buf, size len, size offset);
static ssize fat32_file_write(object_t *obj, const void *buf, size len, size offset);
static int fat32_file_stat(object_t *obj, stat_t *st);
static int fat32_dir_readdir(object_t *obj, void *buf, uint32 count, uint32 *index);
static object_t *fat32_dir_lookup(object_t *obj, const char *name);
static int fat32_dir_stat(object_t *obj, stat_t *st);
static int fat32_node_close(object_t *obj);
static int fat32_update_dirent(fat32_fs_t *fs, uint32 dir_cluster, uint64 entry_offset, const fat32_dirent_t *ent);
static int fat32_find_child(fat32_fs_t *fs, uint32 dir_cluster, const char *name, fat32_dirent_t *ent_out, uint64 *offset_out);

static object_ops_t fat32_file_ops = {
    .read = fat32_file_read,
    .write = fat32_file_write,
    .close = fat32_node_close,
    .readdir = NULL,
    .lookup = NULL,
    .stat = fat32_file_stat
};

static object_ops_t fat32_dir_ops = {
    .read = NULL,
    .write = NULL,
    .close = fat32_node_close,
    .readdir = fat32_dir_readdir,
    .lookup = fat32_dir_lookup,
    .stat = fat32_dir_stat
};

static object_t *fat32_make_object(fat32_node_t *node) {
    if (!node) return NULL;
    return object_create(node->is_dir ? OBJECT_DIR : OBJECT_FILE,
                         node->is_dir ? &fat32_dir_ops : &fat32_file_ops,
                         node);
}

typedef struct {
    //LFN chunks are stored by sequence number
    char fragments[FAT32_LFN_MAX_ENTRIES][14]; //one 13 char chunk each
    bool have_fragment[FAT32_LFN_MAX_ENTRIES]; //seen this chunk yet
    uint8 seq_count;                           //total entries in chain
    uint8 checksum;                            //short name checksum
    bool active;                               //currently assembling one name
} fat32_lfn_state_t;

static void fat32_lfn_state_reset(fat32_lfn_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static uint8 fat32_lfn_checksum(const uint8 short_name[11]) {
    uint8 sum = 0;
    for (size i = 0; i < 11; i++) {
        sum = (uint8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

static void fat32_lfn_copy_piece(const uint16 *src, size src_count, char *dst, size dst_len, size *out_len) {
    size pos = 0;
    if (!src || !dst || dst_len == 0) {
        if (out_len) *out_len = 0;
        return;
    }

    for (size i = 0; i < src_count; i++) {
        uint16 ch = src[i];
        if (ch == 0x0000 || ch == 0xFFFF) break;
        if (pos + 1 >= dst_len) break;
        dst[pos++] = (ch <= 0x7F) ? (char)ch : '?';
    }
    dst[pos] = '\0';
    if (out_len) *out_len = pos;
}

static void fat32_lfn_capture_entry(fat32_lfn_state_t *state, const fat32_lfn_dirent_t *ent) {
    if (!state || !ent) return;

    uint16 name1[5];
    uint16 name2[6];
    uint16 name3[2];
    memcpy(name1, ent->name1, sizeof(name1));
    memcpy(name2, ent->name2, sizeof(name2));
    memcpy(name3, ent->name3, sizeof(name3));

    uint8 seq = ent->order & 0x1F;
    if (seq == 0 || seq > FAT32_LFN_MAX_ENTRIES) {
        fat32_lfn_state_reset(state);
        return;
    }

    if (ent->order & 0x40) {
        //first LFN slot on disk so reset and start fresh
        fat32_lfn_state_reset(state);
        state->active = true;
        state->seq_count = seq;
        state->checksum = ent->checksum;
    }

    if (!state->active || seq > state->seq_count) {
        fat32_lfn_state_reset(state);
        return;
    }

    fat32_lfn_copy_piece(name1, 5, state->fragments[seq - 1], sizeof(state->fragments[seq - 1]), NULL);
    if (seq == state->seq_count) {
        fat32_lfn_copy_piece(name2, 6, state->fragments[seq - 1] + strlen(state->fragments[seq - 1]),
                             sizeof(state->fragments[seq - 1]) - strlen(state->fragments[seq - 1]), NULL);
        fat32_lfn_copy_piece(name3, 2, state->fragments[seq - 1] + strlen(state->fragments[seq - 1]),
                             sizeof(state->fragments[seq - 1]) - strlen(state->fragments[seq - 1]), NULL);
    } else {
        char *dst = state->fragments[seq - 1];
        size dst_len = sizeof(state->fragments[seq - 1]);
        size cur_len = strlen(dst);
        fat32_lfn_copy_piece(name2, 6, dst + cur_len, dst_len - cur_len, NULL);
        cur_len = strlen(dst);
        fat32_lfn_copy_piece(name3, 2, dst + cur_len, dst_len - cur_len, NULL);
    }
    state->have_fragment[seq - 1] = true;
}

static void fat32_component_from_short_name(const fat32_dirent_t *ent, char *out, size out_len);

static int fat32_lfn_assemble(const fat32_lfn_state_t *state, const fat32_dirent_t *short_ent,
                              char *out, size out_len) {
    if (!state || !short_ent || !out || out_len == 0 || !state->active || state->seq_count == 0) return 0;
    if (fat32_lfn_checksum((const uint8 *)short_ent->name) != state->checksum) return 0;

    size pos = 0;
    for (uint8 seq = 1; seq <= state->seq_count; seq++) {
        if (!state->have_fragment[seq - 1]) return 0;
        const char *piece = state->fragments[seq - 1];
        size piece_len = strlen(piece);
        if (piece_len == 0) continue;
        if (pos + piece_len >= out_len) {
            piece_len = out_len - pos - 1;
        }
        memcpy(out + pos, piece, piece_len);
        pos += piece_len;
        if (pos + 1 >= out_len) break;
    }

    out[pos] = '\0';
    return pos > 0;
}

static void fat32_display_name_from_entry(const fat32_lfn_state_t *lfn, const fat32_dirent_t *ent,
                                          char *out, size out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    if (lfn && fat32_lfn_assemble(lfn, ent, out, out_len)) {
        return;
    }

    fat32_component_from_short_name(ent, out, out_len);
}

static inline uint32 fat32_cluster_eoc(uint32 cluster) {
    //FAT entries keep the top 4 bits reserved
    return (cluster & FAT32_EOC_MASK) >= FAT32_EOC_MIN;
}

static inline uint64 align_down_u64(uint64 value, uint64 align) {
    //round down to an alignment boundary
    return value - (value % align);
}

static inline uint64 align_up_u64(uint64 value, uint64 align) {
    //round up to an alignment boundary
    uint64 mod = value % align;
    if (!mod) return value;
    return value + (align - mod);
}

static inline uint32 fat32_cluster_to_sector(fat32_fs_t *fs, uint32 cluster) {
    //cluster 2 is the first data cluster
    return (uint32)(fs->data_offset / fs->bytes_per_sector) +
           (cluster - 2) * fs->sectors_per_cluster;
}

static inline uint64 fat32_cluster_offset(fat32_fs_t *fs, uint32 cluster) {
    //byte offset of the cluster in the device
    return fs->data_offset + (uint64)(cluster - 2) * fs->cluster_size;
}

static int fat32_dev_read_bytes(fat32_fs_t *fs, uint64 offset, void *buf, size len) {
    if (!fs || !fs->source || !buf || len == 0) return -1;

    //round reads to device sector size
    uint64 dev_align = fs->dev_sector_size ? fs->dev_sector_size : 512;
    uint64 aligned_start = align_down_u64(offset, dev_align);
    uint64 aligned_end = align_up_u64(offset + len, dev_align);
    size aligned_len = (size)(aligned_end - aligned_start);
    size pages = (aligned_len + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc(pages);
    if (!phys) return -1;
    void *tmp = P2V(phys);
    memset(tmp, 0, aligned_len);

    ssize rd = object_read(fs->source, tmp, aligned_len, aligned_start);
    if (rd < 0) {
        pmm_free(phys, pages);
        return -1;
    }

    memcpy(buf, (uint8 *)tmp + (offset - aligned_start), len);
    pmm_free(phys, pages);
    return 0;
}

static int fat32_dev_write_bytes(fat32_fs_t *fs, uint64 offset, const void *buf, size len) {
    if (!fs || !fs->source || !buf || len == 0) return -1;

    //read modify write so bytes outside the request stay intact
    uint64 dev_align = fs->dev_sector_size ? fs->dev_sector_size : 512;
    uint64 aligned_start = align_down_u64(offset, dev_align);
    uint64 aligned_end = align_up_u64(offset + len, dev_align);
    size aligned_len = (size)(aligned_end - aligned_start);
    size pages = (aligned_len + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc(pages);
    if (!phys) return -1;
    void *tmp = P2V(phys);
    memset(tmp, 0, aligned_len);

    if (object_read(fs->source, tmp, aligned_len, aligned_start) < 0) {
        pmm_free(phys, pages);
        return -1;
    }

    memcpy((uint8 *)tmp + (offset - aligned_start), buf, len);

    if (object_write(fs->source, tmp, aligned_len, aligned_start) < 0) {
        pmm_free(phys, pages);
        return -1;
    }

    pmm_free(phys, pages);
    return 0;
}

static uint32 fat32_fat_read_entry(fat32_fs_t *fs, uint32 cluster) {
    uint32 entry = 0;
    uint64 offset = fs->fat_offset + ((uint64)cluster * 4);
    if (fat32_dev_read_bytes(fs, offset, &entry, sizeof(entry)) < 0) return FAT32_EOC_MASK;
    return entry & FAT32_EOC_MASK;
}

static int fat32_fat_write_entry(fat32_fs_t *fs, uint32 cluster, uint32 value) {
    //write both fat copies
    value &= FAT32_EOC_MASK;
    for (uint32 copy = 0; copy < fs->fat_count; copy++) {
        uint64 offset = fs->fat_offset + ((uint64)copy * fs->fat_size_sectors * fs->bytes_per_sector) + ((uint64)cluster * 4);
        if (fat32_dev_write_bytes(fs, offset, &value, sizeof(value)) < 0) {
            return -1;
        }
    }
    return 0;
}

static uint32 fat32_cluster_next(fat32_fs_t *fs, uint32 cluster) {
    //follow one link in the chain
    return fat32_fat_read_entry(fs, cluster);
}

static int fat32_cluster_zero(fat32_fs_t *fs, uint32 cluster) {
    //new clusters start blank so old data does not leak back out
    void *zero = kzalloc(fs->cluster_size);
    if (!zero) return -1;
    int ret = fat32_dev_write_bytes(fs, fat32_cluster_offset(fs, cluster), zero, fs->cluster_size);
    kfree(zero);
    return ret;
}

static int fat32_cluster_read(fat32_fs_t *fs, uint32 cluster, void *buf) {
    return fat32_dev_read_bytes(fs, fat32_cluster_offset(fs, cluster), buf, fs->cluster_size);
}

static int fat32_cluster_write(fat32_fs_t *fs, uint32 cluster, const void *buf) {
    return fat32_dev_write_bytes(fs, fat32_cluster_offset(fs, cluster), buf, fs->cluster_size);
}

static uint32 fat32_chain_length(fat32_fs_t *fs, uint32 first_cluster, uint32 *last_cluster) {
    if (first_cluster < 2) {
        if (last_cluster) *last_cluster = 0;
        return 0;
    }

    //count chain length and remember the tail
    uint32 count = 0;
    uint32 cluster = first_cluster;
    uint32 last = cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        count++;
        last = cluster;
        uint32 next = fat32_cluster_next(fs, cluster);
        if (next == cluster) break;
        cluster = next;
        if (count > fs->total_clusters + 1) break;
    }

    if (last_cluster) *last_cluster = last;
    return count;
}

static uint32 fat32_alloc_cluster(fat32_fs_t *fs) {
    //start from the last hint and wrap once
    uint32 start = fs->next_free_cluster < 2 ? 2 : fs->next_free_cluster;
    for (uint32 pass = 0; pass < 2; pass++) {
        uint32 begin = (pass == 0) ? start : 2;
        uint32 end = (pass == 0) ? (fs->total_clusters + 2) : start;
        for (uint32 cluster = begin; cluster < end; cluster++) {
            if (fat32_fat_read_entry(fs, cluster) == 0) {
                if (fat32_fat_write_entry(fs, cluster, FAT32_EOC_MASK) < 0) return 0;
                if (fat32_cluster_zero(fs, cluster) < 0) {
                    fat32_fat_write_entry(fs, cluster, 0);
                    return 0;
                }
                fs->next_free_cluster = cluster + 1;
                return cluster;
            }
        }
    }
    return 0;
}

static int fat32_chain_ensure(fat32_fs_t *fs, fat32_node_t *node, uint32 wanted_clusters) {
    if (!fs || !node || wanted_clusters == 0) return -1;

    //grow the chain until it can hold the requested size
    uint32 last = 0;
    uint32 have = fat32_chain_length(fs, node->first_cluster, &last);
    if (have >= wanted_clusters) return 0;

    while (have < wanted_clusters) {
        uint32 cluster = fat32_alloc_cluster(fs);
        if (!cluster) return -1;

        if (node->first_cluster < 2) {
            node->first_cluster = cluster;
            last = cluster;
        } else {
            if (fat32_fat_write_entry(fs, last, cluster) < 0) return -1;
            last = cluster;
        }
        have++;
    }

    if (fat32_fat_write_entry(fs, last, FAT32_EOC_MASK) < 0) return -1;
    return 0;
}

static int fat32_dir_scan(fat32_fs_t *fs, uint32 start_cluster, uint32 *visible_index,
                          const char *match_name, fat32_dirent_t *match_out, uint64 *match_offset_out) {
    if (!fs || start_cluster < 2) return -1;

    size entries_per_cluster = fs->cluster_size / sizeof(fat32_dirent_t);
    size cluster_bytes = fs->cluster_size;
    void *buf_phys = pmm_alloc((cluster_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!buf_phys) return -1;
    void *buf = P2V(buf_phys);

    uint32 cluster = start_cluster;
    uint64 dir_offset = 0;
    fat32_lfn_state_t lfn_state;
    fat32_lfn_state_reset(&lfn_state);
    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (fat32_cluster_read(fs, cluster, buf) < 0) {
            pmm_free(buf_phys, (cluster_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
            return -1;
        }

        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        for (size i = 0; i < entries_per_cluster; i++, dir_offset += sizeof(fat32_dirent_t)) {
            fat32_dirent_t *ent = &ents[i];

            //0x00 means end of directory
            if ((uint8)ent->name[0] == 0x00) {
                pmm_free(buf_phys, (cluster_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
                return 0;
            }
            //0xe5 means deleted
            if ((uint8)ent->name[0] == 0xE5) continue;
            if (ent->attr == FAT32_ATTR_LFN) {
                fat32_lfn_capture_entry(&lfn_state, (const fat32_lfn_dirent_t *)ent);
                continue;
            }
            if (ent->attr & FAT32_ATTR_VOLUME) continue;

            char display_name[FAT32_MAX_PATH];
            fat32_display_name_from_entry(&lfn_state, ent, display_name, sizeof(display_name));

            if (visible_index) {
                if (match_name) {
                    if (strcmp(display_name, match_name) == 0) {
                        if (match_out) memcpy(match_out, ent, sizeof(*match_out));
                        if (match_offset_out) *match_offset_out = dir_offset;
                        pmm_free(buf_phys, (cluster_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
                        return 1;
                    }
                } else {
                    if (*visible_index == 0) {
                        if (match_out) memcpy(match_out, ent, sizeof(*match_out));
                        if (match_offset_out) *match_offset_out = dir_offset;
                        pmm_free(buf_phys, (cluster_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
                        return 1;
                    }
                    (*visible_index)--;
                }
            }

            fat32_lfn_state_reset(&lfn_state);
        }

        uint32 next = fat32_cluster_next(fs, cluster);
        if (next == cluster) break;
        cluster = next;
    }

    pmm_free(buf_phys, (cluster_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    return 0;
}

static int fat32_path_split(const char *path, char *parent_out, char *base_out) {
    if (!path || !base_out) return -1;

    //split once at the last slash
    size len = strlen(path);
    if (len == 0) {
        base_out[0] = '\0';
        if (parent_out) parent_out[0] = '\0';
        return 0;
    }

    const char *last = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last = p;
    }
    if (!last) {
        if (parent_out) parent_out[0] = '\0';
        if (len >= FAT32_MAX_PATH) return -1;
        memcpy(base_out, path, len + 1);
        return 0;
    }

    size base_len = strlen(last + 1);
    if (base_len == 0 || base_len >= FAT32_MAX_PATH) return -1;
    memcpy(base_out, last + 1, base_len + 1);

    if (parent_out) {
        size parent_len = last - path;
        if (parent_len >= FAT32_MAX_PATH) return -1;
        memcpy(parent_out, path, parent_len);
        parent_out[parent_len] = '\0';
    }
    return 0;
}

static int fat32_short_name_from_component(const char *component, char out[11]) {
    if (!component || !out) return -1;

    //turn one path piece into a 8.3 name if possible
    size len = strlen(component);
    if (len == 0 || len >= FAT32_MAX_NAME) return -1;

    memset(out, ' ', 11);

    size base_len = len;
    size ext_len = 0;
    const char *dot = strchr(component, '.');
    if (dot) {
        base_len = dot - component;
        ext_len = strlen(dot + 1);
        if (strchr(dot + 1, '.')) return -1;
    }

    if (base_len == 0 || base_len > 8 || ext_len > 3) return -1;

    for (size i = 0; i < base_len; i++) {
        char c = component[i];
        if (c == ' ' || c == '/' || c == '\\') return -1;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }

    if (dot) {
        for (size i = 0; i < ext_len; i++) {
            char c = dot[1 + i];
            if (c == ' ' || c == '/' || c == '\\') return -1;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[8 + i] = c;
        }
    }

    return 0;
}

static void fat32_component_from_short_name(const fat32_dirent_t *ent, char *out, size out_len) {
    if (!ent || !out || out_len == 0) return;

    //convert the on-disk 8.3 bytes back into something readable
    char base[9];
    char ext[4];
    size base_len = 0;
    size ext_len = 0;

    for (size i = 0; i < 8 && ent->name[i] != ' '; i++) base[base_len++] = (char)ent->name[i];
    for (size i = 0; i < 3 && ent->name[8 + i] != ' '; i++) ext[ext_len++] = (char)ent->name[8 + i];

    for (size i = 0; i < base_len; i++) {
        if (base[i] >= 'A' && base[i] <= 'Z') base[i] = (char)(base[i] - 'A' + 'a');
    }
    for (size i = 0; i < ext_len; i++) {
        if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] = (char)(ext[i] - 'A' + 'a');
    }

    if (ext_len > 0) {
        snprintf(out, out_len, "%.*s.%.*s", (int)base_len, base, (int)ext_len, ext);
    } else {
        snprintf(out, out_len, "%.*s", (int)base_len, base);
    }
}

static size fat32_lfn_entry_count(const char *name) {
    //13 chars per LFN slot rounded up
    size len = strlen(name);
    return (len + 12) / 13;
}

static int fat32_dir_cluster_for_offset(fat32_fs_t *fs, uint32 start_cluster, uint64 offset,
                                        uint32 *cluster_out, uint64 *cluster_offset_out) {
    if (!fs || start_cluster < 2) return -1;

    //walk the directory chain until the byte offset falls inside one cluster
    uint32 cluster = start_cluster;
    uint64 remaining = offset;
    while (remaining >= fs->cluster_size) {
        uint32 next = fat32_cluster_next(fs, cluster);
        if (next < 2 || fat32_cluster_eoc(next) || next == cluster) return -1;
        cluster = next;
        remaining -= fs->cluster_size;
    }

    if (cluster_out) *cluster_out = cluster;
    if (cluster_offset_out) *cluster_offset_out = remaining;
    return 0;
}

static int fat32_dir_reserve_slots(fat32_fs_t *fs, uint32 dir_cluster, uint32 needed_slots,
                                   uint64 *offset_out, uint32 *cluster_out) {
    if (!fs || dir_cluster < 2 || needed_slots == 0) return -1;

    size entries_per_cluster = fs->cluster_size / sizeof(fat32_dirent_t);
    size cluster_pages = (fs->cluster_size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc(cluster_pages);
    if (!phys) return -1;
    void *buf = P2V(phys);

    uint32 cluster = dir_cluster;
    uint32 last_cluster = dir_cluster;
    uint64 dir_offset = 0;
    uint64 run_start_offset = 0;
    uint32 run_start_cluster = dir_cluster;
    uint32 run_len = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (fat32_cluster_read(fs, cluster, buf) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        for (size i = 0; i < entries_per_cluster; i++, dir_offset += sizeof(fat32_dirent_t)) {
            fat32_dirent_t *ent = &ents[i];
            bool free_slot = ((uint8)ent->name[0] == 0x00 || (uint8)ent->name[0] == 0xE5);

            if (free_slot) {
                if (run_len == 0) {
                    //start of a free run
                    run_start_offset = dir_offset;
                    run_start_cluster = cluster;
                }
                run_len++;
                if (run_len >= needed_slots) {
                    if (offset_out) *offset_out = run_start_offset;
                    if (cluster_out) *cluster_out = run_start_cluster;
                    pmm_free(phys, cluster_pages);
                    return 0;
                }

                if ((uint8)ent->name[0] == 0x00) {
                    //0x00 means the rest is free and we may need new clusters
                    size remaining_here = entries_per_cluster - i - 1;
                    run_len += (uint32)remaining_here;
                    dir_offset += remaining_here * sizeof(fat32_dirent_t);
                    if (run_len >= needed_slots) {
                        if (offset_out) *offset_out = run_start_offset;
                        if (cluster_out) *cluster_out = run_start_cluster;
                        pmm_free(phys, cluster_pages);
                        return 0;
                    }
                    last_cluster = cluster;
                    goto extend_chain;
                }
                continue;
            }

            run_len = 0;
        }

        last_cluster = cluster;
        uint32 next = fat32_cluster_next(fs, cluster);
        if (next == cluster) break;
        cluster = next;
    }

extend_chain:
    if (run_len == 0) {
        run_start_offset = dir_offset;
        run_start_cluster = last_cluster;
    }

    //extend the directory chain if we ran out of room
    uint32 remaining_needed = (needed_slots > run_len) ? (needed_slots - run_len) : 0;
    uint32 extra_clusters = remaining_needed ? (uint32)((remaining_needed + entries_per_cluster - 1) / entries_per_cluster) : 0;
    uint32 prev_cluster = last_cluster;
    uint32 first_new_cluster = 0;

    while (extra_clusters-- > 0) {
        uint32 new_cluster = fat32_alloc_cluster(fs);
        if (!new_cluster) {
            pmm_free(phys, cluster_pages);
            return -1;
        }
        if (!first_new_cluster) first_new_cluster = new_cluster;
        if (fat32_fat_write_entry(fs, prev_cluster, new_cluster) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }
        prev_cluster = new_cluster;
    }

    if (cluster_out) {
        if (run_len > 0) {
            *cluster_out = run_start_cluster;
        } else {
            *cluster_out = first_new_cluster;
        }
    }
    if (offset_out) *offset_out = run_start_offset;
    pmm_free(phys, cluster_pages);
    return 0;
}

static int fat32_dir_write_entry_at(fat32_fs_t *fs, uint32 dir_cluster, uint64 entry_offset,
                                    const fat32_dirent_t *ent) {
    uint32 cluster = 0;
    uint64 cluster_off = 0;
    if (fat32_dir_cluster_for_offset(fs, dir_cluster, entry_offset, &cluster, &cluster_off) < 0) return -1;
    return fat32_update_dirent(fs, cluster, cluster_off, ent);
}

static int fat32_lfn_fill_entry(fat32_lfn_dirent_t *out, uint8 order, uint8 checksum,
                                const char *name, size start_index) {
    if (!out || !name) return -1;

    uint16 name1[5];
    uint16 name2[6];
    uint16 name3[2];
    memset(name1, 0xFF, sizeof(name1));
    memset(name2, 0xFF, sizeof(name2));
    memset(name3, 0xFF, sizeof(name3));

    out->order = order;
    out->attr = FAT32_ATTR_LFN;
    out->type = 0;
    out->checksum = checksum;
    out->first_cluster_lo = 0;

    //pack the 13 char lfn slice across three arrays
    uint16 *parts[3] = { name1, name2, name3 };
    size counts[3] = { 5, 6, 2 };
    size pos = start_index;
    for (size p = 0; p < 3; p++) {
        for (size i = 0; i < counts[p]; i++) {
            //pack one lfn chunk
            uint16 ch = 0xFFFF;
            if (name[pos] != '\0') {
                ch = (uint8)name[pos++];
            } else if (i == 0 && p == 0) {
                ch = 0x0000;
            } else {
                ch = 0xFFFF;
            }
            parts[p][i] = ch;
            if (ch == 0x0000) {
                for (size rem = i + 1; rem < counts[p]; rem++) parts[p][rem] = 0xFFFF;
                for (size remp = p + 1; remp < 3; remp++) {
                    for (size rem = 0; rem < counts[remp]; rem++) parts[remp][rem] = 0xFFFF;
                }
                return 0;
            }
        }
    }

    memcpy(out->name1, name1, sizeof(name1));
    memcpy(out->name2, name2, sizeof(name2));
    memcpy(out->name3, name3, sizeof(name3));
    return 0;
}

static int fat32_short_alias_for_long_name(fat32_fs_t *fs, uint32 parent_cluster, const char *name, char out[11]) {
    if (!fs || !name || !out) return -1;

    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    size base_len = dot ? (size)(dot - name) : strlen(name);
    size ext_len = dot ? strlen(dot + 1) : 0;
    char base[32];
    char ext[8];
    size bi = 0;
    size ei = 0;

    for (size i = 0; i < base_len && bi < sizeof(base) - 1; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            base[bi++] = c;
        }
    }
    base[bi] = '\0';

    for (size i = 0; i < ext_len && ei < sizeof(ext) - 1; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            ext[ei++] = c;
        }
    }
    ext[ei] = '\0';

    if (bi == 0) {
        memcpy(base, "FILE", 5);
        bi = 4;
    }

    for (uint32 suffix = 1; suffix < 100000; suffix++) {
        //build name~n aliases until one is free
        char short_name[12];
        memset(short_name, ' ', 11);
        short_name[11] = '\0';

        char suffix_buf[16];
        snprintf(suffix_buf, sizeof(suffix_buf), "~%u", suffix);
        size suffix_len = strlen(suffix_buf);
        if (suffix_len >= 8) continue;

        size prefix_len = 8 - suffix_len;
        if (prefix_len > bi) prefix_len = bi;
        if (prefix_len == 0) continue;

        memcpy(short_name, base, prefix_len);
        memcpy(short_name + prefix_len, suffix_buf, suffix_len);
        memcpy(short_name + 8, ext, ei > 3 ? 3 : ei);

        fat32_dirent_t ent;
        memset(&ent, 0, sizeof(ent));
        memcpy(ent.name, short_name, 11);
        char alias_name[FAT32_MAX_PATH];
        fat32_component_from_short_name(&ent, alias_name, sizeof(alias_name));
        if (fat32_find_child(fs, parent_cluster, alias_name, NULL, NULL) < 0) {
            memcpy(out, short_name, 11);
            return 0;
        }
    }

    return -1;
}

static int fat32_update_dirent(fat32_fs_t *fs, uint32 dir_cluster, uint64 entry_offset, const fat32_dirent_t *ent) {
    //entry_offset is relative to dir_cluster, so we add the cluster base here
    return fat32_dev_write_bytes(fs, fat32_cluster_offset(fs, dir_cluster) + entry_offset, ent, sizeof(*ent));
}

static int fat32_read_dirent(fat32_fs_t *fs, uint32 dir_cluster, uint64 entry_offset, fat32_dirent_t *ent) {
    //same offset math as update_dirent
    return fat32_dev_read_bytes(fs, fat32_cluster_offset(fs, dir_cluster) + entry_offset, ent, sizeof(*ent));
}

static int fat32_find_entry(fat32_fs_t *fs, uint32 dir_cluster, const char *name, fat32_dirent_t *ent_out, uint64 *offset_out) {
    uint32 dummy = 0;
    if (fat32_dir_scan(fs, dir_cluster, &dummy, name, ent_out, offset_out) < 0) return -1;
    return ent_out && ent_out->name[0] != 0 ? 0 : -1;
}

static fat32_node_t *fat32_node_from_entry(fat32_fs_t *fs, const fat32_dirent_t *ent,
                                           uint32 parent_cluster, uint64 dir_entry_offset, bool is_root) {
    //translate one dirent into the in-memory node object
    fat32_node_t *node = kzalloc(sizeof(fat32_node_t));
    if (!node) return NULL;

    node->fs = fs;
    node->size = ent ? ent->size : 0;
    node->attr = ent ? ent->attr : FAT32_ATTR_DIR;
    node->first_cluster = ent ? (((uint32)ent->first_cluster_hi << 16) | ent->first_cluster_lo) : fs->root_cluster;
    node->parent_cluster = parent_cluster;
    node->dir_entry_offset = dir_entry_offset;
    node->is_dir = (ent ? (ent->attr & FAT32_ATTR_DIR) != 0 : true);
    node->is_root = is_root;
    return node;
}

static int fat32_dir_empty(fat32_fs_t *fs, uint32 dir_cluster) {
    //only . and .. are allowed in a non-empty directory
    size entries_per_cluster = fs->cluster_size / sizeof(fat32_dirent_t);
    size cluster_pages = (fs->cluster_size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc(cluster_pages);
    if (!phys) return -1;
    void *buf = P2V(phys);

    uint32 cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (fat32_cluster_read(fs, cluster, buf) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        for (size i = 0; i < entries_per_cluster; i++) {
            fat32_dirent_t *ent = &ents[i];
            if ((uint8)ent->name[0] == 0x00) {
                pmm_free(phys, cluster_pages);
                return 1;
            }
            if ((uint8)ent->name[0] == 0xE5) continue;
            if (ent->attr == FAT32_ATTR_LFN) continue;
            if (ent->attr & FAT32_ATTR_VOLUME) continue;
            if ((ent->name[0] == '.' && ent->name[1] == ' ' && ent->name[2] == ' ')) continue;
            if ((ent->name[0] == '.' && ent->name[1] == '.' && ent->name[2] == ' ')) continue;
            pmm_free(phys, cluster_pages);
            return 0;
        }

        uint32 next = fat32_cluster_next(fs, cluster);
        if (next == cluster) break;
        cluster = next;
    }

    pmm_free(phys, cluster_pages);
    return 1;
}

static int fat32_update_file_meta(fat32_fs_t *fs, fat32_node_t *node) {
    if (!fs || !node || node->is_root) return 0;

    //write back size and first cluster to the parent dirent
    fat32_dirent_t ent;
    if (fat32_read_dirent(fs, node->parent_cluster, node->dir_entry_offset, &ent) < 0) return -1;
    ent.first_cluster_hi = (uint16)((node->first_cluster >> 16) & 0xFFFF);
    ent.first_cluster_lo = (uint16)(node->first_cluster & 0xFFFF);
    ent.size = node->size;
    return fat32_update_dirent(fs, node->parent_cluster, node->dir_entry_offset, &ent);
}

static ssize fat32_file_read(object_t *obj, void *buf, size len, size offset) {
    fat32_node_t *node = (fat32_node_t *)obj->data;
    if (!node || !node->fs || !buf) return -1;
    if (node->is_dir) return -1;
    if (node->first_cluster < 2 || offset >= node->size) return 0;

    size remaining = len;
    if (offset + remaining > node->size) remaining = node->size - offset;

    size cluster_pages = (node->fs->cluster_size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc(cluster_pages);
    if (!phys) return -1;
    void *tmp = P2V(phys);

    size copied = 0;
    uint64 pos = offset;
    while (remaining > 0) {
        //walk to the cluster that contains the current offset
        uint32 cluster_index = (uint32)(pos / node->fs->cluster_size);
        uint32 cluster_off = (uint32)(pos % node->fs->cluster_size);
        uint32 cluster = node->first_cluster;

        for (uint32 i = 0; i < cluster_index; i++) {
            cluster = fat32_cluster_next(node->fs, cluster);
            if (cluster < 2 || fat32_cluster_eoc(cluster)) {
                pmm_free(phys, cluster_pages);
                return (ssize)copied;
            }
        }

        if (fat32_cluster_read(node->fs, cluster, tmp) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        size chunk = node->fs->cluster_size - cluster_off;
        if (chunk > remaining) chunk = remaining;
        memcpy((uint8 *)buf + copied, (uint8 *)tmp + cluster_off, chunk);
        copied += chunk;
        remaining -= chunk;
        pos += chunk;
    }

    pmm_free(phys, cluster_pages);
    return (ssize)copied;
}

static int fat32_file_write_range(fat32_node_t *node, size offset, const void *buf, size len) {
    fat32_fs_t *fs = node->fs;
    size remaining = len;
    size written = 0;
    uint64 pos = offset;

    size cluster_pages = (fs->cluster_size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc(cluster_pages);
    if (!phys) return -1;
    void *tmp = P2V(phys);

    while (remaining > 0) {
        //same cluster walk as read
        uint32 cluster_index = (uint32)(pos / fs->cluster_size);
        uint32 cluster_off = (uint32)(pos % fs->cluster_size);
        uint32 cluster = node->first_cluster;

        for (uint32 i = 0; i < cluster_index; i++) {
            uint32 next = fat32_cluster_next(fs, cluster);
            if (next < 2 || fat32_cluster_eoc(next)) {
                //chain ended early, caller should have extended it
                next = 0;
            }
            cluster = next ? next : cluster;
            if (!next) break;
        }

        if (cluster < 2 || fat32_cluster_eoc(cluster)) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        if (fat32_cluster_read(fs, cluster, tmp) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        size chunk = fs->cluster_size - cluster_off;
        if (chunk > remaining) chunk = remaining;
        memcpy((uint8 *)tmp + cluster_off, (const uint8 *)buf + written, chunk);
        if (fat32_cluster_write(fs, cluster, tmp) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        written += chunk;
        remaining -= chunk;
        pos += chunk;
    }

    pmm_free(phys, cluster_pages);
    return 0;
}

static ssize fat32_file_write(object_t *obj, const void *buf, size len, size offset) {
    fat32_node_t *node = (fat32_node_t *)obj->data;
    if (!node || !node->fs || !buf) return -1;
    if (node->is_dir) return -1;
    if (len == 0) return 0;

    uint64 end = (uint64)offset + len;
    uint32 needed_clusters = (uint32)((end + node->fs->cluster_size - 1) / node->fs->cluster_size);
    if (needed_clusters == 0) needed_clusters = 1;

    if (node->first_cluster < 2) {
        //brand new file, give it its first cluster now
        uint32 first = fat32_alloc_cluster(node->fs);
        if (!first) return -1;
        node->first_cluster = first;
        if (fat32_update_file_meta(node->fs, node) < 0) return -1;
    }

    if (fat32_chain_ensure(node->fs, node, needed_clusters) < 0) return -1;

    //zero fill sparse gap first
    if (offset > node->size) {
        size gap = offset - node->size;
        void *zero = kzalloc(gap);
        if (!zero) return -1;
        if (fat32_file_write_range(node, node->size, zero, gap) < 0) {
            kfree(zero);
            return -1;
        }
        kfree(zero);
    }

    if (fat32_file_write_range(node, offset, buf, len) < 0) return -1;

    if (end > node->size) {
        node->size = end;
        if (fat32_update_file_meta(node->fs, node) < 0) return -1;
    }

    return (ssize)len;
}

static int fat32_file_stat(object_t *obj, stat_t *st) {
    fat32_node_t *node = (fat32_node_t *)obj->data;
    if (!node || !st) return -1;
    //files report their byte size, dirs do not
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_FILE;
    st->size = node->size;
    return 0;
}

static int fat32_dir_stat(object_t *obj, stat_t *st) {
    fat32_node_t *node = (fat32_node_t *)obj->data;
    if (!node || !st) return -1;
    //dirs are always size 0 here
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DIR;
    st->size = 0;
    return 0;
}

static int fat32_node_close(object_t *obj) {
    //node data is owned by the open object
    if (!obj || !obj->data) return 0;
    kfree(obj->data);
    obj->data = NULL;
    return 0;
}

static int fat32_dir_readdir(object_t *obj, void *buf, uint32 count, uint32 *index) {
    fat32_node_t *node = (fat32_node_t *)obj->data;
    if (!node || !node->fs || !buf || !index || !node->is_dir) return -1;

    dirent_t *entries = (dirent_t *)buf;
    uint32 visible_skip = *index;
    uint32 filled = 0;

    size entries_per_cluster = node->fs->cluster_size / sizeof(fat32_dirent_t);
    size cluster_pages = (node->fs->cluster_size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc(cluster_pages);
    if (!phys) return -1;
    void *tmp = P2V(phys);

    uint32 cluster = node->first_cluster;
    fat32_lfn_state_t lfn_state;
    fat32_lfn_state_reset(&lfn_state);
    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        if (fat32_cluster_read(node->fs, cluster, tmp) < 0) {
            pmm_free(phys, cluster_pages);
            return -1;
        }

        fat32_dirent_t *ents = (fat32_dirent_t *)tmp;
        for (size i = 0; i < entries_per_cluster && filled < count; i++) {
            fat32_dirent_t *ent = &ents[i];
            if ((uint8)ent->name[0] == 0x00) {
                pmm_free(phys, cluster_pages);
                *index += filled;
                return filled;
            }
            if ((uint8)ent->name[0] == 0xE5) continue;
            if (ent->attr == FAT32_ATTR_LFN) {
                fat32_lfn_capture_entry(&lfn_state, (const fat32_lfn_dirent_t *)ent);
                continue;
            }
            if (ent->attr & FAT32_ATTR_VOLUME) continue;

            if (visible_skip > 0) {
                visible_skip--;
                fat32_lfn_state_reset(&lfn_state);
                continue;
            }

            fat32_display_name_from_entry(&lfn_state, ent, entries[filled].name, sizeof(entries[filled].name));
            entries[filled].type = (ent->attr & FAT32_ATTR_DIR) ? FS_TYPE_DIR : FS_TYPE_FILE;
            filled++;
            fat32_lfn_state_reset(&lfn_state);
        }

        uint32 next = fat32_cluster_next(node->fs, cluster);
        if (next == cluster) break;
        cluster = next;
    }

    pmm_free(phys, cluster_pages);
    *index += filled;
    return filled;
}

static object_t *fat32_dir_lookup(object_t *obj, const char *name) {
    //lookup by display name, not raw 8.3 bytes
    fat32_node_t *node = (fat32_node_t *)obj->data;
    if (!node || !node->fs || !name || !node->is_dir) return NULL;

    fat32_dirent_t ent;
    uint64 offset = 0;
    if (fat32_find_entry(node->fs, node->first_cluster, name, &ent, &offset) < 0) return NULL;

    fat32_node_t *child = fat32_node_from_entry(node->fs, &ent, node->first_cluster, offset, false);
    if (!child) return NULL;
    return fat32_make_object(child);
}

static int fat32_find_child(fat32_fs_t *fs, uint32 dir_cluster, const char *name, fat32_dirent_t *ent_out, uint64 *offset_out) {
    //thin wrapper used by create and lookup helpers
    uint32 dummy = 0;
    if (fat32_dir_scan(fs, dir_cluster, &dummy, name, ent_out, offset_out) < 0) return -1;
    if (ent_out && ent_out->name[0] != 0) return 0;
    return -1;
}

static int fat32_lookup_path(fat32_fs_t *fs, const char *path, fat32_dirent_t *ent_out, uint32 *parent_cluster_out, uint64 *entry_offset_out) {
    if (!fs || !path) return -1;

    //root path is handled as a special case
    while (*path == '/') path++;
    if (*path == '\0' || (path[0] == '.' && path[1] == '\0')) {
        if (parent_cluster_out) *parent_cluster_out = fs->root_cluster;
        if (entry_offset_out) *entry_offset_out = 0;
        if (ent_out) memset(ent_out, 0, sizeof(*ent_out));
        return 0;
    }

    uint32 current_cluster = fs->root_cluster;
    char component[FAT32_MAX_PATH];
    const char *p = path;
    fat32_dirent_t ent;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *end = p;
        while (*end && *end != '/') end++;
        size comp_len = (size)(end - p);
        if (comp_len == 0 || comp_len >= sizeof(component)) return -1;

        //copy one component at a time
        memcpy(component, p, comp_len);
        component[comp_len] = '\0';

        uint64 entry_offset = 0;
        if (fat32_find_child(fs, current_cluster, component, &ent, &entry_offset) < 0) return -1;

        const char *next = end;
        while (*next == '/') next++;
        bool is_last = (*next == '\0');

        if (is_last) {
            if (ent_out) memcpy(ent_out, &ent, sizeof(ent));
            if (parent_cluster_out) *parent_cluster_out = current_cluster;
            if (entry_offset_out) *entry_offset_out = entry_offset;
            return 0;
        }

        if (!(ent.attr & FAT32_ATTR_DIR)) return -1;
        current_cluster = ((uint32)ent.first_cluster_hi << 16) | ent.first_cluster_lo;
        if (current_cluster < 2) return -1;
        p = end;
    }

    return -1;
}

static int fat32_dir_remove_entry_chain(fat32_fs_t *fs, uint32 parent_cluster, uint64 entry_offset) {
    if (!fs) return -1;

    //delete the short entry and any lfn slots right before it
    uint32 cluster = 0;
    uint64 cluster_off = 0;
    if (fat32_dir_cluster_for_offset(fs, parent_cluster, entry_offset, &cluster, &cluster_off) < 0) return -1;

    fat32_dirent_t ent;
    if (fat32_read_dirent(fs, cluster, cluster_off, &ent) < 0) return -1;

    uint64 cur_offset = entry_offset;
    while (cur_offset >= sizeof(fat32_dirent_t)) {
        uint64 prev_offset = cur_offset - sizeof(fat32_dirent_t);
        uint32 prev_cluster = 0;
        uint64 prev_cluster_off = 0;
        if (fat32_dir_cluster_for_offset(fs, parent_cluster, prev_offset, &prev_cluster, &prev_cluster_off) < 0) break;

        fat32_dirent_t prev;
        if (fat32_read_dirent(fs, prev_cluster, prev_cluster_off, &prev) < 0) break;
        if (prev.attr != FAT32_ATTR_LFN) break;
        prev.name[0] = (char)0xE5;
        if (fat32_update_dirent(fs, prev_cluster, prev_cluster_off, &prev) < 0) return -1;
        cur_offset = prev_offset;
    }

    ent.name[0] = (char)0xE5;
    return fat32_update_dirent(fs, cluster, cluster_off, &ent);
}

static int fat32_free_chain(fat32_fs_t *fs, uint32 first_cluster) {
    //clear every fat link in the chain
    uint32 cluster = first_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        uint32 next = fat32_cluster_next(fs, cluster);
        fat32_fat_write_entry(fs, cluster, 0);
        if (next == cluster) break;
        cluster = next;
    }
    return 0;
}

static int fat32_fs_stat_path(fat32_fs_t *fs, const char *path, stat_t *st) {
    if (!fs || !path || !st) return -1;

    //stat the root without touching lookup_path
    while (*path == '/') path++;
    if (*path == '\0' || (path[0] == '.' && path[1] == '\0')) {
        memset(st, 0, sizeof(stat_t));
        st->type = FS_TYPE_DIR;
        st->size = 0;
        return 0;
    }

    fat32_dirent_t ent;
    if (fat32_lookup_path(fs, path, &ent, NULL, NULL) < 0) return -1;

    memset(st, 0, sizeof(stat_t));
    if (ent.attr & FAT32_ATTR_DIR) {
        st->type = FS_TYPE_DIR;
        st->size = 0;
    } else {
        st->type = FS_TYPE_FILE;
        st->size = ent.size;
    }
    return 0;
}

static object_t *fat32_fs_lookup(fs_t *fs_obj, const char *path) {
    fat32_fs_t *fs = (fat32_fs_t *)fs_obj->data;
    if (!fs) return NULL;

    //root returns a synthetic node since it has no parent dirent
    fat32_dirent_t ent;
    uint32 parent_cluster = 0;
    uint64 entry_offset = 0;

    if (!path || *path == '\0' || (path[0] == '.' && path[1] == '\0')) {
        fat32_node_t *node = fat32_node_from_entry(fs, NULL, 0, 0, true);
        if (!node) return NULL;
        node->first_cluster = fs->root_cluster;
        node->is_dir = true;
        node->is_root = true;
        return fat32_make_object(node);
    }

    if (fat32_lookup_path(fs, path, &ent, &parent_cluster, &entry_offset) < 0) return NULL;

    fat32_node_t *node = fat32_node_from_entry(fs, &ent, parent_cluster, entry_offset, false);
    if (!node) return NULL;
    return fat32_make_object(node);
}

static int fat32_fs_create(fs_t *fs_obj, const char *path, uint32 type) {
    fat32_fs_t *fs = (fat32_fs_t *)fs_obj->data;
    if (!fs || !path || !*path) return -1;

    //split parent and leaf before we touch the directory
    char parent_path[FAT32_MAX_PATH];
    char name[FAT32_MAX_PATH];
    if (fat32_path_split(path, parent_path, name) < 0) return -1;

    uint32 parent_cluster = fs->root_cluster;
    if (parent_path[0]) {
        fat32_dirent_t parent_ent;
        if (fat32_lookup_path(fs, parent_path, &parent_ent, NULL, NULL) < 0) return -1;
        if (!(parent_ent.attr & FAT32_ATTR_DIR)) return -1;
        parent_cluster = ((uint32)parent_ent.first_cluster_hi << 16) | parent_ent.first_cluster_lo;
    }

    if (fat32_find_child(fs, parent_cluster, name, NULL, NULL) == 0) return -1;

    char short_name[11];
    bool needs_lfn = false;
    if (fat32_short_name_from_component(name, short_name) < 0) {
        //not representable as 8.3, so we need LFN slots too
        needs_lfn = true;
    }

    uint64 entry_offset = 0;
    uint32 slot_cluster = 0;
    uint32 slots_needed = 1;
    if (needs_lfn) {
        //LFN entry count plus the short alias slot
        slots_needed = (uint32)fat32_lfn_entry_count(name) + 1;
        if (slots_needed <= 1 || slots_needed > FAT32_LFN_MAX_ENTRIES + 1) return -1;
        if (fat32_short_alias_for_long_name(fs, parent_cluster, name, short_name) < 0) return -1;

        char alias_display[FAT32_MAX_PATH];
        fat32_dirent_t alias_probe;
        memset(&alias_probe, 0, sizeof(alias_probe));
        memcpy(alias_probe.name, short_name, 11);
        fat32_component_from_short_name(&alias_probe, alias_display, sizeof(alias_display));
        if (fat32_find_child(fs, parent_cluster, alias_display, NULL, NULL) == 0) return -1;
    }

    if (fat32_dir_reserve_slots(fs, parent_cluster, slots_needed, &entry_offset, &slot_cluster) < 0) return -1;

    fat32_dirent_t short_ent;
    memset(&short_ent, 0, sizeof(short_ent));
    memcpy(short_ent.name, short_name, 11);
    short_ent.attr = (type == FS_TYPE_DIR) ? FAT32_ATTR_DIR : FAT32_ATTR_ARCHIVE;

    if (needs_lfn) {
        uint8 checksum = fat32_lfn_checksum((const uint8 *)short_name);
        uint32 lfn_count = (uint32)fat32_lfn_entry_count(name);

        //LFN slots are written backwards on disk
        for (uint32 seq = lfn_count; seq > 0; seq--) {
            fat32_lfn_dirent_t lfn;
            uint8 order = (uint8)seq;
            if (seq == lfn_count) order |= 0x40;
            if (fat32_lfn_fill_entry(&lfn, order, checksum, name, (seq - 1) * 13) < 0) return -1;
            if (fat32_dir_write_entry_at(fs, parent_cluster, entry_offset + (uint64)(lfn_count - seq) * sizeof(fat32_dirent_t),
                                         (const fat32_dirent_t *)&lfn) < 0) return -1;
        }
        entry_offset += (uint64)lfn_count * sizeof(fat32_dirent_t);
    }

    if (type == FS_TYPE_DIR) {
        uint32 new_dir_cluster = fat32_alloc_cluster(fs);
        if (!new_dir_cluster) return -1;

        //new dirs get their own cluster and dotdot records
        short_ent.first_cluster_hi = (uint16)((new_dir_cluster >> 16) & 0xFFFF);
        short_ent.first_cluster_lo = (uint16)(new_dir_cluster & 0xFFFF);

        if (fat32_dir_write_entry_at(fs, parent_cluster, entry_offset, &short_ent) < 0) return -1;

        fat32_dirent_t dot;
        fat32_dirent_t dotdot;
        memset(&dot, 0, sizeof(dot));
        memset(&dotdot, 0, sizeof(dotdot));
        memcpy(dot.name, ".          ", 11);
        dot.attr = FAT32_ATTR_DIR;
        dot.first_cluster_hi = (uint16)((new_dir_cluster >> 16) & 0xFFFF);
        dot.first_cluster_lo = (uint16)(new_dir_cluster & 0xFFFF);

        memcpy(dotdot.name, "..         ", 11);
        dotdot.attr = FAT32_ATTR_DIR;
        dotdot.first_cluster_hi = (uint16)((parent_cluster >> 16) & 0xFFFF);
        dotdot.first_cluster_lo = (uint16)(parent_cluster & 0xFFFF);

        //zero the new dir cluster first
        if (fat32_cluster_zero(fs, new_dir_cluster) < 0) return -1;
        if (fat32_dev_write_bytes(fs, fat32_cluster_offset(fs, new_dir_cluster), &dot, sizeof(dot)) < 0) return -1;
        if (fat32_dev_write_bytes(fs, fat32_cluster_offset(fs, new_dir_cluster) + sizeof(dot), &dotdot, sizeof(dotdot)) < 0) return -1;
        return 0;
    }

    if (fat32_dir_write_entry_at(fs, parent_cluster, entry_offset, &short_ent) < 0) return -1;
    return 0;
}

static int fat32_fs_remove(fs_t *fs_obj, const char *path) {
    fat32_fs_t *fs = (fat32_fs_t *)fs_obj->data;
    if (!fs || !path || !*path) return -1;

    //split parent from leaf so we can delete the right dirent
    char parent_path[FAT32_MAX_PATH];
    char name[FAT32_MAX_PATH];
    if (fat32_path_split(path, parent_path, name) < 0) return -1;

    uint32 parent_cluster = fs->root_cluster;
    if (parent_path[0]) {
        fat32_dirent_t parent_ent;
        if (fat32_lookup_path(fs, parent_path, &parent_ent, NULL, NULL) < 0) return -1;
        if (!(parent_ent.attr & FAT32_ATTR_DIR)) return -1;
        parent_cluster = ((uint32)parent_ent.first_cluster_hi << 16) | parent_ent.first_cluster_lo;
    }

    fat32_dirent_t ent;
    uint64 entry_offset = 0;
    if (fat32_find_child(fs, parent_cluster, name, &ent, &entry_offset) < 0) return -1;

    if (ent.attr & FAT32_ATTR_DIR) {
        //dirs must be empty before removal
        uint32 dir_cluster = ((uint32)ent.first_cluster_hi << 16) | ent.first_cluster_lo;
        if (dir_cluster == fs->root_cluster) return -1;
        if (fat32_dir_empty(fs, dir_cluster) != 1) return -1;
        fat32_free_chain(fs, dir_cluster);
    } else {
        uint32 first_cluster = ((uint32)ent.first_cluster_hi << 16) | ent.first_cluster_lo;
        if (first_cluster >= 2) {
            fat32_free_chain(fs, first_cluster);
        }
    }

    return fat32_dir_remove_entry_chain(fs, parent_cluster, entry_offset);
}

static int fat32_fs_stat(fs_t *fs_obj, const char *path, stat_t *st) {
    fat32_fs_t *fs = (fat32_fs_t *)fs_obj->data;
    if (!fs || !st) return -1;
    //just forward into the path helper
    return fat32_fs_stat_path(fs, path, st);
}

static fs_ops_t fat32_ops = {
    .lookup = fat32_fs_lookup,  //path lookup
    .create = fat32_fs_create,  //create file or dir
    .remove = fat32_fs_remove,  //delete file or dir
    .readdir = NULL,
    .stat = fat32_fs_stat       //path stat
};

intptr fat32_mount(object_t *source, const char *target) {
    if (!source || !target) return -1;

    block_device_info_t info = {0};
    if (object_get_info(source, OBJ_INFO_BLOCK_DEVICE, &info, sizeof(info)) < 0 || info.sector_size == 0) {
        info.sector_size = 512;
    }

    if (info.sector_size < 512 || info.sector_size > 4096 || (info.sector_size & (info.sector_size - 1)) != 0) {
        return -1;
    }

    size boot_pages = (info.sector_size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *boot_phys = pmm_alloc(boot_pages);
    if (!boot_phys) return -1;
    void *boot = P2V(boot_phys);

    if (object_read(source, boot, info.sector_size, 0) < 0) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)boot;
    //basic BPB sanity checks
    if (bpb->bytes_per_sector == 0 || (bpb->bytes_per_sector & (bpb->bytes_per_sector - 1)) != 0) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }
    if (bpb->sectors_per_cluster == 0 || (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) != 0) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }
    if (bpb->fat_count == 0 || bpb->fat_size_32 == 0 || bpb->root_cluster < 2) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    uint32 total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    if (total_sectors == 0) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    uint32 fat_size = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    if (fat_size == 0) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    uint32 reserved = bpb->reserved_sectors;
    uint32 data_sectors = total_sectors - reserved - (bpb->fat_count * fat_size);
    if (data_sectors == 0) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    fat32_fs_t *state = kzalloc(sizeof(fat32_fs_t));
    if (!state) {
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    fs_t *fs = kzalloc(sizeof(fs_t));
    if (!fs) {
        kfree(state);
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    state->source = source;
    object_ref(source);
    state->dev_sector_size = info.sector_size;
    state->bytes_per_sector = bpb->bytes_per_sector;
    state->sectors_per_cluster = bpb->sectors_per_cluster;
    state->reserved_sectors = reserved;
    state->fat_count = bpb->fat_count;
    state->fat_size_sectors = fat_size;
    state->root_cluster = bpb->root_cluster;
    state->fsinfo_sector = bpb->fsinfo_sector;
    state->total_sectors = total_sectors;
    state->total_clusters = data_sectors / bpb->sectors_per_cluster;
    state->fat_offset = (uint64)reserved * bpb->bytes_per_sector;
    state->data_offset = (uint64)(reserved + (bpb->fat_count * fat_size)) * bpb->bytes_per_sector;
    state->cluster_size = (uint64)bpb->bytes_per_sector * bpb->sectors_per_cluster;
    state->next_free_cluster = 2;
    state->fs = fs;

    //cache the derived geometry now
    if (state->total_clusters < 1 || state->cluster_size == 0) {
        object_deref(source);
        kfree(fs);
        kfree(state);
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    fs->name = "fat32";
    fs->ops = &fat32_ops;
    fs->data = state;

    if (fs_mount_register(target, fs) < 0) {
        object_deref(source);
        kfree(fs);
        kfree(state);
        pmm_free(boot_phys, boot_pages);
        return -1;
    }

    pmm_free(boot_phys, boot_pages);
    printf("[fat32] mounted %s: %u bytes/sector, %u sectors/cluster, root cluster %u\n",
           target, state->bytes_per_sector, state->sectors_per_cluster, state->root_cluster);
    return 0;
}
