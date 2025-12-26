#ifndef FS_DA_H
#define FS_DA_H

#include <arch/types.h>

/*
 * Delta Archive (DA) Format
 * See docs/fs/archive.md for specification.
 */

//DA magic: 'D' 'A' 0x00 0x01
#define DA_MAGIC 0x44410001

//DA flags
#define DA_FLAG_SORTED  (1 << 0)  //entries sorted by path
#define DA_FLAG_HASHED  (1 << 1)  //path hashes included

//DA entry types
#define DA_TYPE_FILE    0
#define DA_TYPE_DIR     1
#define DA_TYPE_LINK    2

//DA header (40 bytes)
typedef struct {
    uint32 magic;          //0x44410001
    uint32 checksum;       //CRC32 of header+entry table
    uint16 version;        //format version (0x0001)
    uint16 flags;          //archive flags
    uint32 entry_count;    //number of entries
    uint32 entry_off;      //offset to entry table
    uint32 strtab_off;     //offset to string table
    uint32 strtab_size;    //size of string table
    uint32 data_off;       //offset to file data section
    uint64 total_size;     //total uncompressed size
} __attribute__((packed)) da_header_t;

//DA entry (32 bytes)
typedef struct {
    uint32 path_off;       //offset in string table to path
    uint32 flags;          //entry flags (type in bits 0-3)
    uint64 data_off;       //offset from data section start
    uint64 size;           //size in bytes (0 for directories)
    uint32 hash;           //path hash (FNV-1a)
    uint32 reserved;       //must be zero
} __attribute__((packed)) da_entry_t;

//error codes
#define DA_OK           0
#define DA_ERR_MAGIC   -1
#define DA_ERR_VERSION -2
#define DA_ERR_BOUNDS  -3
#define DA_ERR_NOTFOUND -4

//validation
int da_validate(void *archive, size archive_size);

//get entry count
uint32 da_entry_count(da_header_t *hdr);

//get entry by index
da_entry_t *da_get_entry(da_header_t *hdr, uint32 index);

//get path of an entry
const char *da_entry_path(da_header_t *hdr, da_entry_t *entry);

//get type of an entry
static inline uint32 da_entry_type(da_entry_t *entry) {
    return entry->flags & 0xF;
}

//get file data pointer (only valid for DA_TYPE_FILE)
void *da_file_data(da_header_t *hdr, da_entry_t *entry);

//find entry by path (returns NULL if not found)
da_entry_t *da_find(da_header_t *hdr, const char *path);

//FNV-1a hash (for path lookup)
uint32 da_hash(const char *path);

//iterate callback type
typedef void (*da_iter_fn)(da_header_t *hdr, da_entry_t *entry, void *ctx);

//iterate all entries
void da_foreach(da_header_t *hdr, da_iter_fn fn, void *ctx);

#endif
