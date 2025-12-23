#ifndef _DB_H
#define _DB_H

#include <stdint.h>

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

//use format-specific entry point (e.x ELF entry)
#define DB_ENTRY_USE_FORMAT 0xFFFFFFFF

struct db_request_header {
    uint32_t magic;         //DB_REQUEST_MAGIC
    uint32_t checksum;      //CRC32 (computed with this field as 0)
    uint16_t version;       //protocol version (0x0001)
    uint16_t header_size;   //size of header + all request tags
    uint32_t flags;         //request flags
    uint32_t entry_point;   //offset of entry or DB_ENTRY_USE_FORMAT
} __attribute__((packed));

//request tags
#define DB_RTAG_END               0x0000
#define DB_RTAG_FRAMEBUFFER_PREF  0x0001
#define DB_RTAG_MIN_MEMORY        0x0002
#define DB_RTAG_LOAD_ADDRESS      0x0003
#define DB_RTAG_STACK_SIZE        0x0004

struct db_request_tag {
    uint16_t type;
    uint16_t flags;
    uint32_t size;          //TOTAL size of this tag
} __attribute__((packed));

struct db_rtag_framebuffer_pref {
    uint16_t type;          //0x0001
    uint16_t flags;         //bit 0 required
    uint32_t size;          //24
    uint32_t min_width;
    uint32_t min_height;
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint8_t  min_bpp;
    uint8_t  preferred_bpp;
    uint8_t  padding[2];
} __attribute__((packed));

struct db_rtag_min_memory {
    uint16_t type;          //0x0002
    uint16_t flags;
    uint32_t size;          //16
    uint64_t min_bytes;
} __attribute__((packed));

struct db_rtag_stack_size {
    uint16_t type;          //0x0004
    uint16_t flags;
    uint32_t size;          //16
    uint64_t stack_size;
} __attribute__((packed));

//boot info (passed to kernel)
struct db_boot_info {
    uint32_t magic;         //DB_BOOT_INFO_MAGIC
    uint32_t total_size;    //total size including all tags
    uint32_t version;       //protocol version
    uint32_t reserved;
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
    uint16_t type;
    uint16_t flags;
    uint32_t size;          //total size of this tag
} __attribute__((packed));

//DB_TAG_END
struct db_tag_end {
    uint16_t type;          //0x0000
    uint16_t flags;
    uint32_t size;          //8
} __attribute__((packed));

//DB_TAG_CMDLINE
struct db_tag_cmdline {
    uint16_t type;          //0x0001
    uint16_t flags;
    uint32_t size;
    char cmdline[];         //null-terminated UTF-8
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
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} __attribute__((packed));

struct db_tag_memory_map {
    uint16_t type;          //0x0002
    uint16_t flags;
    uint32_t size;
    uint32_t entry_size;    //sizeof(db_mmap_entry)
    uint32_t entry_count;
    struct db_mmap_entry entries[];
} __attribute__((packed));

//DB_TAG_FRAMEBUFFER
struct db_tag_framebuffer {
    uint16_t type;          //0x0003
    uint16_t flags;
    uint32_t size;
    uint64_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  red_shift;
    uint8_t  red_size;
    uint8_t  green_shift;
    uint8_t  green_size;
    uint8_t  blue_shift;
    uint8_t  blue_size;
    uint8_t  reserved_shift;
    uint8_t  reserved_size;
    uint8_t  padding[3];
} __attribute__((packed));

//DB_TAG_ACPI_RSDP
struct db_tag_acpi_rsdp {
    uint16_t type;          //0x0005
    uint16_t flags;         //bit 0: is XSDP
    uint32_t size;
    uint64_t rsdp_address;
} __attribute__((packed));

//DB_TAG_BOOTLOADER
struct db_tag_bootloader {
    uint16_t type;          //0x0008
    uint16_t flags;
    uint32_t size;
    char name[];            //null-terminated
} __attribute__((packed));

//DB_TAG_BOOT_TIME
struct db_tag_boot_time {
    uint16_t type;          //0x0007
    uint16_t flags;
    uint32_t size;
    uint64_t unix_timestamp;
} __attribute__((packed));

//DB_TAG_KERNEL_FILE
struct db_tag_kernel_file {
    uint16_t type;          //0x0009
    uint16_t flags;
    uint32_t size;
    char path[];            //null-terminated
} __attribute__((packed));

//DB_TAG_EFI_SYSTEM_TABLE
struct db_tag_efi_system_table {
    uint16_t type;          //0x000A
    uint16_t flags;
    uint32_t size;
    uint64_t system_table;  //pointer to EFI_SYSTEM_TABLE
} __attribute__((packed));

//DB_TAG_INITRD
struct db_tag_initrd {
    uint16_t type;          //0x000B
    uint16_t flags;
    uint32_t size;
    uint64_t start;
    uint64_t length;
} __attribute__((packed));

//DB_TAG_KERNEL_PHYS
struct db_tag_kernel_phys {
    uint16_t type;          //0x000C
    uint16_t flags;
    uint32_t size;
    uint64_t phys_base;
    uint64_t phys_length;
} __attribute__((packed));

//macros

//iterate over tags in bootinfo
#define DB_FOREACH_TAG(info, tag) \
    for (struct db_tag *tag = (struct db_tag *)((uint8_t *)(info) + sizeof(struct db_boot_info)); \
         tag->type != DB_TAG_END; \
         tag = (struct db_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7)))

//align to 8 bytes
#define DB_ALIGN8(x) (((x) + 7) & ~7)

//align to 4 bytes
#define DB_ALIGN4(x) (((x) + 3) & ~3)

#endif
