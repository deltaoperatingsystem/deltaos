#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

//DA magic: 'D' 'A' 0x00 0x01
#define DA_MAGIC 0x44410001

//DA flags
#define DA_FLAG_SORTED  (1 << 0)
#define DA_FLAG_HASHED  (1 << 1)

//DA entry types
#define DA_TYPE_FILE    0
#define DA_TYPE_DIR     1
#define DA_TYPE_LINK    2

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t checksum;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t entry_off;
    uint32_t strtab_off;
    uint32_t strtab_size;
    uint32_t data_off;
    uint64_t total_size;
} da_header_t;

typedef struct {
    uint32_t path_off;
    uint32_t flags;
    uint64_t data_off;
    uint64_t size;
    uint32_t hash;
    uint32_t reserved;
} da_entry_t;

#pragma pack(pop)

//build-time entry for create command
typedef struct build_entry {
    char *path;             //absolute path in archive (e.g., "/bin/init")
    char *src_path;         //source path on disk
    uint32_t type;
    uint64_t size;
    char *link_target;      //for symlinks
    struct build_entry *next;
} build_entry_t;

//FNV-1a hash
static uint32_t fnv1a_hash(const char *s) {
    uint32_t hash = 0x811c9dc5;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= 0x01000193;
    }
    return hash;
}

//CRC32 (standard polynomial)
static uint32_t crc32_table[256];
static bool crc32_init = false;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_init = true;
}

static uint32_t crc32(const void *data, size_t len) {
    if (!crc32_init) crc32_init_table();
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

//compare function for qsort (sort entries by path)
static int entry_cmp(const void *a, const void *b) {
    const build_entry_t *const *ea = a;
    const build_entry_t *const *eb = b;
    return strcmp((*ea)->path, (*eb)->path);
}

//normalize path (remove trailing slashes, ensure starts with /)
static char *normalize_path(const char *base, const char *rel) {
    size_t base_len = strlen(base);
    size_t rel_len = strlen(rel);
    
    //calc size: "/" + rel (skip base)
    char *result = malloc(rel_len - base_len + 2);
    if (!result) return NULL;
    
    if (rel_len == base_len) {
        //root directory
        strcpy(result, "/");
    } else {
        //skip base, keep rest
        sprintf(result, "%s", rel + base_len);
        //ensure starts with /
        if (result[0] != '/') {
            memmove(result + 1, result, strlen(result) + 1);
            result[0] = '/';
        }
    }
    
    //remove trailing slash (except for root)
    size_t len = strlen(result);
    if (len > 1 && result[len - 1] == '/') {
        result[len - 1] = '\0';
    }
    
    return result;
}

//recursively walk directory and build entry list
static int walk_dir(const char *base, const char *path, build_entry_t **list, uint32_t *count) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "error: cannot open directory '%s': %s\n", path, strerror(errno));
        return -1;
    }
    
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
        
        struct stat st;
        if (lstat(full_path, &st) < 0) {
            fprintf(stderr, "warning: cannot stat '%s': %s\n", full_path, strerror(errno));
            continue;
        }
        
        build_entry_t *e = calloc(1, sizeof(build_entry_t));
        if (!e) {
            closedir(d);
            return -1;
        }
        
        e->path = normalize_path(base, full_path);
        e->src_path = strdup(full_path);
        
        if (S_ISDIR(st.st_mode)) {
            e->type = DA_TYPE_DIR;
            e->size = 0;
            
            //add to list
            e->next = *list;
            *list = e;
            (*count)++;
            
            //recurse
            if (walk_dir(base, full_path, list, count) < 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISLNK(st.st_mode)) {
            e->type = DA_TYPE_LINK;
            e->size = 0;
            
            char target[4096];
            ssize_t len = readlink(full_path, target, sizeof(target) - 1);
            if (len < 0) {
                fprintf(stderr, "warning: cannot read symlink '%s'\n", full_path);
                free(e->path);
                free(e->src_path);
                free(e);
                continue;
            }
            target[len] = '\0';
            e->link_target = strdup(target);
            
            e->next = *list;
            *list = e;
            (*count)++;
        } else if (S_ISREG(st.st_mode)) {
            e->type = DA_TYPE_FILE;
            e->size = st.st_size;
            
            e->next = *list;
            *list = e;
            (*count)++;
        } else {
            //skip special files
            free(e->path);
            free(e->src_path);
            free(e);
        }
    }
    
    closedir(d);
    return 0;
}

static uint64_t align8(uint64_t v) {
    return (v + 7) & ~7ULL;
}

//create command
static int cmd_create(const char *archive, const char *source) {
    //check source is a directory
    struct stat st;
    if (stat(source, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a directory\n", source);
        return 1;
    }
    
    //get absolute path of source (without trailing slash)
    char abs_source[4096];
    if (!realpath(source, abs_source)) {
        fprintf(stderr, "error: cannot resolve path '%s'\n", source);
        return 1;
    }
    size_t len = strlen(abs_source);
    if (len > 1 && abs_source[len - 1] == '/') {
        abs_source[len - 1] = '\0';
    }
    
    //add root entry
    build_entry_t *list = calloc(1, sizeof(build_entry_t));
    list->path = strdup("/");
    list->src_path = strdup(abs_source);
    list->type = DA_TYPE_DIR;
    uint32_t count = 1;
    
    //walk directory tree
    printf("Scanning %s...\n", abs_source);
    if (walk_dir(abs_source, abs_source, &list, &count) < 0) {
        return 1;
    }
    printf("Found %u entries\n", count);
    
    //convert list to array and sort
    build_entry_t **arr = malloc(count * sizeof(build_entry_t *));
    build_entry_t *e = list;
    for (uint32_t i = 0; i < count; i++) {
        arr[i] = e;
        e = e->next;
    }
    qsort(arr, count, sizeof(build_entry_t *), entry_cmp);
    
    //build string table
    size_t strtab_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        strtab_size += strlen(arr[i]->path) + 1;
        if (arr[i]->type == DA_TYPE_LINK && arr[i]->link_target) {
            strtab_size += strlen(arr[i]->link_target) + 1;
        }
    }
    
    char *strtab = malloc(strtab_size);
    size_t strtab_pos = 0;
    
    //assign path offsets
    uint32_t *path_offs = malloc(count * sizeof(uint32_t));
    uint32_t *link_offs = malloc(count * sizeof(uint32_t));  //for symlinks
    
    for (uint32_t i = 0; i < count; i++) {
        path_offs[i] = strtab_pos;
        size_t plen = strlen(arr[i]->path) + 1;
        memcpy(strtab + strtab_pos, arr[i]->path, plen);
        strtab_pos += plen;
        
        if (arr[i]->type == DA_TYPE_LINK && arr[i]->link_target) {
            link_offs[i] = strtab_pos;
            size_t tlen = strlen(arr[i]->link_target) + 1;
            memcpy(strtab + strtab_pos, arr[i]->link_target, tlen);
            strtab_pos += tlen;
        } else {
            link_offs[i] = 0;
        }
    }
    
    //calculate offsets
    uint32_t header_size = sizeof(da_header_t);
    uint32_t entry_off = header_size;
    uint32_t entries_size = count * sizeof(da_entry_t);
    uint32_t strtab_off = entry_off + entries_size;
    uint32_t data_off = align8(strtab_off + strtab_size);
    
    //build data section and entry table
    da_entry_t *entries = calloc(count, sizeof(da_entry_t));
    
    //first pass: calculate data offsets
    uint64_t data_pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        entries[i].path_off = path_offs[i];
        entries[i].flags = arr[i]->type;
        entries[i].hash = fnv1a_hash(arr[i]->path);
        entries[i].reserved = 0;
        
        if (arr[i]->type == DA_TYPE_FILE) {
            entries[i].data_off = data_pos;
            entries[i].size = arr[i]->size;
            data_pos = align8(data_pos + arr[i]->size);
        } else if (arr[i]->type == DA_TYPE_LINK) {
            entries[i].data_off = link_offs[i];  //points to strtab
            entries[i].size = 0;
        } else {
            entries[i].data_off = 0;
            entries[i].size = 0;
        }
    }
    
    uint64_t total_data_size = data_pos;
    
    //build header
    da_header_t hdr = {
        .magic = DA_MAGIC,
        .checksum = 0,  //computed later
        .version = 0x0001,
        .flags = DA_FLAG_SORTED | DA_FLAG_HASHED,
        .entry_count = count,
        .entry_off = entry_off,
        .strtab_off = strtab_off,
        .strtab_size = strtab_size,
        .data_off = data_off,
        .total_size = total_data_size
    };
    
    //compute checksum over header + entries (with checksum field = 0)
    size_t check_size = sizeof(da_header_t) + entries_size;
    uint8_t *check_buf = malloc(check_size);
    memcpy(check_buf, &hdr, sizeof(da_header_t));
    memcpy(check_buf + sizeof(da_header_t), entries, entries_size);
    hdr.checksum = crc32(check_buf, check_size);
    free(check_buf);
    
    //write archive
    FILE *f = fopen(archive, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot create '%s': %s\n", archive, strerror(errno));
        return 1;
    }
    
    //write header
    fwrite(&hdr, sizeof(da_header_t), 1, f);
    
    //write entries
    fwrite(entries, sizeof(da_entry_t), count, f);
    
    //write string table
    fwrite(strtab, 1, strtab_size, f);
    
    //pad to data section
    uint32_t pad_size = data_off - (strtab_off + strtab_size);
    if (pad_size > 0) {
        uint8_t zeros[8] = {0};
        fwrite(zeros, 1, pad_size, f);
    }
    
    //write file data
    printf("Writing data...\n");
    for (uint32_t i = 0; i < count; i++) {
        if (arr[i]->type != DA_TYPE_FILE) continue;
        
        FILE *src = fopen(arr[i]->src_path, "rb");
        if (!src) {
            fprintf(stderr, "error: cannot read '%s'\n", arr[i]->src_path);
            fclose(f);
            return 1;
        }
        
        char buf[65536];
        size_t remaining = arr[i]->size;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            size_t r = fread(buf, 1, chunk, src);
            if (r != chunk) {
                fprintf(stderr, "error: short read from '%s'\n", arr[i]->src_path);
                fclose(src);
                fclose(f);
                return 1;
            }
            fwrite(buf, 1, chunk, f);
            remaining -= chunk;
        }
        fclose(src);
        
        //pad to 8-byte alignment
        size_t aligned = align8(arr[i]->size);
        if (aligned > arr[i]->size) {
            uint8_t zeros[8] = {0};
            fwrite(zeros, 1, aligned - arr[i]->size, f);
        }
    }
    
    fclose(f);
    
    printf("Created %s (%u entries, %lu bytes data)\n", archive, count, (unsigned long)total_data_size);
    
    //cleanup
    for (uint32_t i = 0; i < count; i++) {
        free(arr[i]->path);
        free(arr[i]->src_path);
        free(arr[i]->link_target);
        free(arr[i]);
    }
    free(arr);
    free(entries);
    free(strtab);
    free(path_offs);
    free(link_offs);
    
    return 0;
}

//list command
static int cmd_list(const char *archive) {
    FILE *f = fopen(archive, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", archive, strerror(errno));
        return 1;
    }
    
    da_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "error: cannot read header\n");
        fclose(f);
        return 1;
    }
    
    if (hdr.magic != DA_MAGIC) {
        fprintf(stderr, "error: invalid magic (expected 0x%08x, got 0x%08x)\n", DA_MAGIC, hdr.magic);
        fclose(f);
        return 1;
    }
    
    //read entries
    da_entry_t *entries = malloc(hdr.entry_count * sizeof(da_entry_t));
    fseek(f, hdr.entry_off, SEEK_SET);
    fread(entries, sizeof(da_entry_t), hdr.entry_count, f);
    
    //read string table
    char *strtab = malloc(hdr.strtab_size);
    fseek(f, hdr.strtab_off, SEEK_SET);
    fread(strtab, 1, hdr.strtab_size, f);
    
    //print entries
    const char *type_names[] = {"FILE", "DIR ", "LINK"};
    for (uint32_t i = 0; i < hdr.entry_count; i++) {
        da_entry_t *e = &entries[i];
        const char *path = strtab + e->path_off;
        uint32_t type = e->flags & 0xF;
        
        if (type == DA_TYPE_LINK) {
            const char *target = strtab + e->data_off;
            printf("%s %s -> %s\n", type_names[type], path, target);
        } else if (type == DA_TYPE_FILE) {
            printf("%s %8lu %s\n", type_names[type], (unsigned long)e->size, path);
        } else {
            printf("%s          %s\n", type_names[type], path);
        }
    }
    
    free(entries);
    free(strtab);
    fclose(f);
    return 0;
}

//extract command
static int cmd_extract(const char *archive, const char *dest) {
    FILE *f = fopen(archive, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", archive, strerror(errno));
        return 1;
    }
    
    da_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != DA_MAGIC) {
        fprintf(stderr, "error: invalid archive\n");
        fclose(f);
        return 1;
    }
    
    //read entries and string table
    da_entry_t *entries = malloc(hdr.entry_count * sizeof(da_entry_t));
    fseek(f, hdr.entry_off, SEEK_SET);
    fread(entries, sizeof(da_entry_t), hdr.entry_count, f);
    
    char *strtab = malloc(hdr.strtab_size);
    fseek(f, hdr.strtab_off, SEEK_SET);
    fread(strtab, 1, hdr.strtab_size, f);
    
    //create destination if needed
    mkdir(dest, 0755);
    
    printf("Extracting to %s...\n", dest);
    
    for (uint32_t i = 0; i < hdr.entry_count; i++) {
        da_entry_t *e = &entries[i];
        const char *path = strtab + e->path_off;
        uint32_t type = e->flags & 0xF;
        
        //build full path
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s%s", dest, path);
        
        if (type == DA_TYPE_DIR) {
            mkdir(full_path, 0755);
        } else if (type == DA_TYPE_LINK) {
            const char *target = strtab + e->data_off;
            unlink(full_path);  //remove if exists
            if (symlink(target, full_path) < 0) {
                fprintf(stderr, "warning: cannot create symlink '%s'\n", full_path);
            }
        } else if (type == DA_TYPE_FILE) {
            FILE *out = fopen(full_path, "wb");
            if (!out) {
                fprintf(stderr, "error: cannot create '%s'\n", full_path);
                continue;
            }
            
            fseek(f, hdr.data_off + e->data_off, SEEK_SET);
            
            char buf[65536];
            size_t remaining = e->size;
            while (remaining > 0) {
                size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
                fread(buf, 1, chunk, f);
                fwrite(buf, 1, chunk, out);
                remaining -= chunk;
            }
            
            fclose(out);
        }
    }
    
    printf("Extracted %u entries\n", hdr.entry_count);
    
    free(entries);
    free(strtab);
    fclose(f);
    return 0;
}

//info command
static int cmd_info(const char *archive) {
    FILE *f = fopen(archive, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", archive, strerror(errno));
        return 1;
    }
    
    da_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "error: cannot read header\n");
        fclose(f);
        return 1;
    }
    
    fclose(f);
    
    if (hdr.magic != DA_MAGIC) {
        fprintf(stderr, "error: invalid magic\n");
        return 1;
    }
    
    printf("Delta Archive: %s\n", archive);
    printf("  Version:      0x%04x\n", hdr.version);
    printf("  Flags:        0x%04x", hdr.flags);
    if (hdr.flags & DA_FLAG_SORTED) printf(" SORTED");
    if (hdr.flags & DA_FLAG_HASHED) printf(" HASHED");
    printf("\n");
    printf("  Entries:      %u\n", hdr.entry_count);
    printf("  Entry offset: 0x%08x\n", hdr.entry_off);
    printf("  Strtab offset:0x%08x (%u bytes)\n", hdr.strtab_off, hdr.strtab_size);
    printf("  Data offset:  0x%08x\n", hdr.data_off);
    printf("  Total size:   %lu bytes\n", (unsigned long)hdr.total_size);
    printf("  Checksum:     0x%08x\n", hdr.checksum);
    
    return 0;
}

static void usage(void) {
    fprintf(stderr, "Usage: darc <command> [args...]\n\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  create <archive.da> <source_dir>  Create archive from directory\n");
    fprintf(stderr, "  list <archive.da>                 List archive contents\n");
    fprintf(stderr, "  extract <archive.da> <dest_dir>   Extract archive\n");
    fprintf(stderr, "  info <archive.da>                 Show archive info\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    
    const char *cmd = argv[1];
    
    if (strcmp(cmd, "create") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: darc create <archive.da> <source_dir>\n");
            return 1;
        }
        return cmd_create(argv[2], argv[3]);
    } else if (strcmp(cmd, "list") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: darc list <archive.da>\n");
            return 1;
        }
        return cmd_list(argv[2]);
    } else if (strcmp(cmd, "extract") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: darc extract <archive.da> <dest_dir>\n");
            return 1;
        }
        return cmd_extract(argv[2], argv[3]);
    } else if (strcmp(cmd, "info") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: darc info <archive.da>\n");
            return 1;
        }
        return cmd_info(argv[2]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage();
        return 1;
    }
}
