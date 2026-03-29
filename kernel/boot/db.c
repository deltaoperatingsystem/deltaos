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

static void db_reset_cache(void) {
    boot_info = NULL;
    cached_fb = NULL;
    cached_mmap = NULL;
    cached_bootloader = NULL;
    cached_cmdline = NULL;
    cached_efi = NULL;
    cached_kernel_phys = NULL;
    cached_initrd = NULL;
    cached_acpi = NULL;
}

void db_parse(struct db_boot_info *info) {
    db_reset_cache();

    if (!info) {
        puts("[db] ERROR: null boot info\n");
        return;
    }
    
    if (info->magic != DB_BOOT_INFO_MAGIC) {
        puts("[db] ERROR: invalid boot info magic\n");
        return;
    }

    if (info->version != DB_PROTOCOL_VERSION) {
        puts("[db] ERROR: unsupported boot info version\n");
        return;
    }

    if (info->total_size < sizeof(struct db_boot_info) + sizeof(struct db_tag_end)) {
        puts("[db] ERROR: boot info too small\n");
        return;
    }
    
    boot_info = info;

    uint8 *base = (uint8 *)info;
    uint8 *end = base + info->total_size;
    uint8 *p = base + sizeof(struct db_boot_info);

    while (p + sizeof(struct db_tag) <= end) {
        struct db_tag *tag = (struct db_tag *)p;
        size aligned = DB_ALIGN8(tag->size);

        if (tag->size < sizeof(struct db_tag) || aligned < tag->size) {
            puts("[db] ERROR: invalid tag size\n");
            db_reset_cache();
            return;
        }
        if (p + aligned > end) {
            puts("[db] ERROR: tag chain exceeds boot info size\n");
            db_reset_cache();
            return;
        }

        switch (tag->type) {
            case DB_TAG_BOOTLOADER:
                if (tag->size >= sizeof(struct db_tag_bootloader) && aligned >= sizeof(struct db_tag_bootloader))
                    cached_bootloader = (struct db_tag_bootloader *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_FRAMEBUFFER:
                if (tag->size >= sizeof(struct db_tag_framebuffer) && aligned >= sizeof(struct db_tag_framebuffer))
                    cached_fb = (struct db_tag_framebuffer *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_MEMORY_MAP:
                if (tag->size >= sizeof(struct db_tag_memory_map) && aligned >= sizeof(struct db_tag_memory_map))
                    cached_mmap = (struct db_tag_memory_map *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_CMDLINE:
                if (tag->size >= sizeof(struct db_tag_cmdline) && aligned >= sizeof(struct db_tag_cmdline))
                    cached_cmdline = (struct db_tag_cmdline *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_EFI_SYSTEM_TABLE:
                if (tag->size >= sizeof(struct db_tag_efi_system_table) && aligned >= sizeof(struct db_tag_efi_system_table))
                    cached_efi = (struct db_tag_efi_system_table *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_KERNEL_PHYS:
                if (tag->size >= sizeof(struct db_tag_kernel_phys) && aligned >= sizeof(struct db_tag_kernel_phys))
                    cached_kernel_phys = (struct db_tag_kernel_phys *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_INITRD:
                if (tag->size >= sizeof(struct db_tag_initrd) && aligned >= sizeof(struct db_tag_initrd))
                    cached_initrd = (struct db_tag_initrd *)tag;
                else goto bad_tag;
                break;
            case DB_TAG_ACPI_RSDP:
                if (tag->size >= sizeof(struct db_tag_acpi_rsdp) && aligned >= sizeof(struct db_tag_acpi_rsdp))
                    cached_acpi = (struct db_tag_acpi_rsdp *)tag;
                else goto bad_tag;
                break;
            default:
                break;
        }

        if (tag->type == DB_TAG_END) {
            return;
        }
        p += aligned;
        continue;
bad_tag:
        puts("[db] ERROR: malformed tag payload\n");
        db_reset_cache();
        return;
    }

    puts("[db] ERROR: boot info missing terminator\n");
    db_reset_cache();
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
