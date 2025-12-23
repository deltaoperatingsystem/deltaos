#ifndef BOOT_DB_H
#define BOOT_DB_H

#include <arch/types.h>

//request header (embedded in kernel)
#define DB_REQUEST_MAGIC    0x44420001  //'D' 'B' 0x00 0x01
#define DB_BOOT_INFO_MAGIC  0x44424F4B  //'D' 'B' 'O' 'K'
#define DB_PROTOCOL_VERSION 0x0001

//request flags
#define DB_REQ_FRAMEBUFFER  (1 << 0)
#define DB_REQ_MEMORY_MAP   (1 << 1)
#define DB_REQ_MODULES      (1 << 2)
#define DB_REQ_ACPI         (1 << 3)
#define DB_REQ_CMDLINE      (1 << 4)
#define DB_REQ_SMP          (1 << 5)
#define DB_REQ_INITRD       (1 << 6)
#define DB_REQ_HAS_TAGS     (1 << 7)

//use format-specific entry point
#define DB_ENTRY_USE_FORMAT 0xFFFFFFFF

struct db_request_header {
    uint32 magic;         //DB_REQUEST_MAGIC
    uint32 checksum;      //CRC32 (computed with this field as 0)
    uint16 version;       //protocol version (0x0001)
    uint16 header_size;   //size of header + all request tags
    uint32 flags;         //request flags
    uint32 entry_point;   //offset of entry or DB_ENTRY_USE_FORMAT
} __attribute__((packed));

//boot info (passed to kernel)
struct db_boot_info {
    uint32 magic;         //DB_BOOT_INFO_MAGIC
    uint32 total_size;    //total size including all tags
    uint32 version;       //protocol version
    uint32 reserved;
} __attribute__((packed));

//boot info tags
#define DB_TAG_END              0x0000
#define DB_TAG_CMDLINE          0x0001
#define DB_TAG_MEMORY_MAP       0x0002
#define DB_TAG_FRAMEBUFFER      0x0003
#define DB_TAG_MODULES          0x0004
#define DB_TAG_ACPI_RSDP        0x0005
#define DB_TAG_SMP              0x0006
#define DB_TAG_BOOT_TIME        0x0007
#define DB_TAG_BOOTLOADER       0x0008
#define DB_TAG_KERNEL_FILE      0x0009
#define DB_TAG_EFI_SYSTEM_TABLE 0x000A
#define DB_TAG_INITRD           0x000B
#define DB_TAG_KERNEL_PHYS      0x000C

struct db_tag {
    uint16 type;
    uint16 flags;
    uint32 size;          //total size of this tag
} __attribute__((packed));

//DB_TAG_END
struct db_tag_end {
    uint16 type;          //0x0000
    uint16 flags;
    uint32 size;          //8
} __attribute__((packed));

//DB_TAG_CMDLINE
struct db_tag_cmdline {
    uint16 type;          //0x0001
    uint16 flags;
    uint32 size;
    char cmdline[];       //null-terminated UTF-8
} __attribute__((packed));

//DB_TAG_MEMORY_MAP
#define DB_MEM_RESERVED         0
#define DB_MEM_USABLE           1
#define DB_MEM_ACPI_RECLAIMABLE 2
#define DB_MEM_ACPI_NVS         3
#define DB_MEM_BAD              4
#define DB_MEM_BOOTLOADER       5
#define DB_MEM_KERNEL           6
#define DB_MEM_FRAMEBUFFER      7
#define DB_MEM_INITRD           8
#define DB_MEM_MODULES          9

struct db_mmap_entry {
    uint64 base;
    uint64 length;
    uint32 type;
    uint32 attributes;
} __attribute__((packed));

struct db_tag_memory_map {
    uint16 type;          //0x0002
    uint16 flags;
    uint32 size;
    uint32 entry_size;    //sizeof(db_mmap_entry)
    uint32 entry_count;
    struct db_mmap_entry entries[];
} __attribute__((packed));

//DB_TAG_FRAMEBUFFER
struct db_tag_framebuffer {
    uint16 type;          //0x0003
    uint16 flags;
    uint32 size;
    uint64 address;
    uint32 width;
    uint32 height;
    uint32 pitch;
    uint8  bpp;
    uint8  red_shift;
    uint8  red_size;
    uint8  green_shift;
    uint8  green_size;
    uint8  blue_shift;
    uint8  blue_size;
    uint8  reserved_shift;
    uint8  reserved_size;
    uint8  padding[3];
} __attribute__((packed));

//DB_TAG_BOOTLOADER
struct db_tag_bootloader {
    uint16 type;          //0x0008
    uint16 flags;
    uint32 size;
    char name[];          //null-terminated
} __attribute__((packed));

//DB_TAG_EFI_SYSTEM_TABLE
struct db_tag_efi_system_table {
    uint16 type;          //0x000A
    uint16 flags;
    uint32 size;
    uint64 system_table;  //pointer to EFI_SYSTEM_TABLE
} __attribute__((packed));

//DB_TAG_ACPI_RSDP
struct db_tag_acpi_rsdp {
    uint16 type;          //0x0005
    uint16 flags;         //bit 0: is XSDP
    uint32 size;
    uint64 rsdp_address;
} __attribute__((packed));

//DB_TAG_INITRD
struct db_tag_initrd {
    uint16 type;          //0x000B
    uint16 flags;
    uint32 size;
    uint64 start;
    uint64 length;
} __attribute__((packed));

//DB_TAG_KERNEL_PHYS
struct db_tag_kernel_phys {
    uint16 type;          //0x000C
    uint16 flags;
    uint32 size;
    uint64 phys_base;
    uint64 phys_length;
} __attribute__((packed));

//DB_TAG_BOOT_TIME
struct db_tag_boot_time {
    uint16 type;          //0x0007
    uint16 flags;
    uint32 size;
    uint64 unix_timestamp;
} __attribute__((packed));

//DB_TAG_KERNEL_FILE
struct db_tag_kernel_file {
    uint16 type;          //0x0009
    uint16 flags;
    uint32 size;
    char path[];
} __attribute__((packed));

//macros
#define DB_ALIGN8(x) (((x) + 7) & ~7)

#define DB_FOREACH_TAG(info, tag) \
    for (struct db_tag *tag = (struct db_tag *)((uint8 *)(info) + sizeof(struct db_boot_info)); \
         tag->type != DB_TAG_END; \
         tag = (struct db_tag *)((uint8 *)tag + DB_ALIGN8(tag->size)))

//parser functions
void db_parse(struct db_boot_info *info);
struct db_boot_info *db_get_boot_info(void);
struct db_tag_framebuffer *db_get_framebuffer(void);
struct db_tag_memory_map *db_get_memory_map(void);
struct db_tag_kernel_phys *db_get_kernel_phys(void);
struct db_tag_initrd *db_get_initrd(void);
struct db_tag_acpi_rsdp *db_get_acpi_rsdp(void);
const char *db_get_bootloader_name(void);
const char *db_get_cmdline(void);

#endif
