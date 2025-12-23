#include <boot/db.h>
#include <lib/io.h>

static struct db_boot_info *boot_info = NULL;
static struct db_tag_framebuffer *cached_fb = NULL;
static struct db_tag_memory_map *cached_mmap = NULL;
static struct db_tag_bootloader *cached_bootloader = NULL;
static struct db_tag_cmdline *cached_cmdline = NULL;
static struct db_tag_efi_system_table *cached_efi = NULL;
static struct db_tag_kernel_phys *cached_kernel_phys = NULL;
static struct db_tag_initrd *cached_initrd = NULL;
static struct db_tag_acpi_rsdp *cached_acpi = NULL;

void db_parse(struct db_boot_info *info) {
    if (!info) {
        puts("[db] ERROR: null boot info\n");
        return;
    }
    
    if (info->magic != DB_BOOT_INFO_MAGIC) {
        puts("[db] ERROR: invalid boot info magic\n");
        return;
    }
    
    boot_info = info;
    
    DB_FOREACH_TAG(info, tag) {
        switch (tag->type) {
            case DB_TAG_BOOTLOADER:
                cached_bootloader = (struct db_tag_bootloader *)tag;
                break;
            case DB_TAG_FRAMEBUFFER:
                cached_fb = (struct db_tag_framebuffer *)tag;
                break;
            case DB_TAG_MEMORY_MAP:
                cached_mmap = (struct db_tag_memory_map *)tag;
                break;
            case DB_TAG_CMDLINE:
                cached_cmdline = (struct db_tag_cmdline *)tag;
                break;
            case DB_TAG_EFI_SYSTEM_TABLE:
                cached_efi = (struct db_tag_efi_system_table *)tag;
                break;
            case DB_TAG_KERNEL_PHYS:
                cached_kernel_phys = (struct db_tag_kernel_phys *)tag;
                break;
            case DB_TAG_INITRD:
                cached_initrd = (struct db_tag_initrd *)tag;
                break;
            case DB_TAG_ACPI_RSDP:
                cached_acpi = (struct db_tag_acpi_rsdp *)tag;
                break;
            default:
                break;
        }
    }
}

struct db_tag_framebuffer *db_get_framebuffer(void) {
    return cached_fb;
}

struct db_tag_memory_map *db_get_memory_map(void) {
    return cached_mmap;
}

struct db_boot_info *db_get_boot_info(void) {
    return boot_info;
}

struct db_tag_kernel_phys *db_get_kernel_phys(void) {
    return cached_kernel_phys;
}

struct db_tag_initrd *db_get_initrd(void) {
    return cached_initrd;
}

struct db_tag_acpi_rsdp *db_get_acpi_rsdp(void) {
    return cached_acpi;
}

const char *db_get_bootloader_name(void) {
    return cached_bootloader ? cached_bootloader->name : NULL;
}

const char *db_get_cmdline(void) {
    return cached_cmdline ? cached_cmdline->cmdline : NULL;
}
