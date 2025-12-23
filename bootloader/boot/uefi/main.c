#include <stdint.h>
#include <stddef.h>
#include <db.h>
#include <string.h>
#include "efi.h"
#include "../src/graphics.h"
#include "../src/console.h"
#include "../src/file.h"
#include "../src/menu.h"
#include "../src/config.h"
#include "../src/elf.h"
#include "../src/paging.h"
#include <crc32.h>

#define DELBOOT_NAME    "DelBoot 0.5"
#define KERNEL_SCAN_SIZE (32 * 1024)
#define CONFIG_PATH     L"\\EFI\\BOOT\\delboot.cfg"

static uint8_t boot_info_buffer[16384] __attribute__((aligned(8)));
typedef void (*KernelEntry)(struct db_boot_info *info);


EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_HANDLE gImageHandle;

static struct db_request_header *find_db_header(void *kernel_data, uint64_t kernel_size) {
    uint64_t scan_size = kernel_size < KERNEL_SCAN_SIZE ? kernel_size : KERNEL_SCAN_SIZE;
    uint8_t *data = kernel_data;
    
    for (uint64_t i = 0; i + sizeof(struct db_request_header) <= scan_size; i += 8) {
        struct db_request_header *hdr = (struct db_request_header *)(data + i);
        if (hdr->magic == DB_REQUEST_MAGIC) {
            //verify checksum
            uint32_t saved = hdr->checksum;
            hdr->checksum = 0;
            uint32_t computed = crc32(hdr, hdr->header_size);
            hdr->checksum = saved;
            
            if (computed == saved) {
                return hdr;
            } else {
                //if we found the magic but checksum failed it's an error
                gfx_clear(COLOR_BG);
                con_set_color(COLOR_RED, 0);
                con_print_at(40, 40, "Error: DB Request Header Checksum Mismatch!");
                con_set_color(COLOR_WHITE, 0);
                con_print_at(40, 80, "Kernel has a DB header but it's not properly patched.");
                gBS->Stall(5000000);
                return (struct db_request_header *)-1;
            }
        }
    }
    return NULL;
}

static uint32_t efi_to_db_memtype(uint32_t efi_type) {
    switch (efi_type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
            return DB_MEM_BOOTLOADER;
        case EfiConventionalMemory:
            return DB_MEM_USABLE;
        case EfiACPIReclaimMemory:
            return DB_MEM_ACPI_RECLAIMABLE;
        case EfiACPIMemoryNVS:
            return DB_MEM_ACPI_NVS;
        case EfiUnusableMemory:
            return DB_MEM_BAD;
        default:
            return DB_MEM_RESERVED;
    }
}

static void *find_acpi_table(EFI_SYSTEM_TABLE *st, EFI_GUID acpi_guid) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (memcmp(&st->ConfigurationTable[i].VendorGuid, &acpi_guid, sizeof(EFI_GUID)) == 0) {
            return st->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

static struct db_boot_info *build_boot_info(
    uint32_t req_flags,
    const char *cmdline,
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    EFI_MEMORY_DESCRIPTOR *mmap,
    UINTN mmap_size,
    UINTN desc_size,
    elf_load_info_t *load_info,
    const char *kernel_path,
    uint64_t initrd_addr,
    uint64_t initrd_size
) {
    struct db_boot_info *info = (struct db_boot_info *)boot_info_buffer;
    uint8_t *ptr = boot_info_buffer + sizeof(struct db_boot_info);
    
    info->magic = DB_BOOT_INFO_MAGIC;
    info->version = DB_PROTOCOL_VERSION;
    info->reserved = 0;
    
    //DB_TAG_BOOTLOADER
    {
        struct db_tag_bootloader *tag = (struct db_tag_bootloader *)ptr;
        size_t name_len = strlen(DELBOOT_NAME) + 1;
        tag->type = DB_TAG_BOOTLOADER;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_bootloader) + name_len;
        memcpy(tag->name, DELBOOT_NAME, name_len);
        ptr += DB_ALIGN8(tag->size);
    }
    
    //dB_TAG_FRAMEBUFFER
    if ((req_flags & DB_REQ_FRAMEBUFFER) && gop) {
        struct db_tag_framebuffer *tag = (struct db_tag_framebuffer *)ptr;
        tag->type = DB_TAG_FRAMEBUFFER;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_framebuffer);
        tag->address = gop->Mode->FrameBufferBase;
        tag->width = gop->Mode->Info->HorizontalResolution;
        tag->height = gop->Mode->Info->VerticalResolution;
        tag->pitch = gop->Mode->Info->PixelsPerScanLine * 4;
        tag->bpp = 32;
        tag->blue_shift = 0; tag->blue_size = 8;
        tag->green_shift = 8; tag->green_size = 8;
        tag->red_shift = 16; tag->red_size = 8;
        tag->reserved_shift = 24; tag->reserved_size = 8;
        tag->padding[0] = tag->padding[1] = tag->padding[2] = 0;
        ptr += DB_ALIGN8(tag->size);
    }
    
    //DB_TAG_MEMORY_MAP
    if ((req_flags & DB_REQ_MEMORY_MAP) && mmap) {
        UINTN entry_count = mmap_size / desc_size;
        struct db_tag_memory_map *tag = (struct db_tag_memory_map *)ptr;
        tag->type = DB_TAG_MEMORY_MAP;
        tag->flags = 0;
        tag->entry_size = sizeof(struct db_mmap_entry);
        tag->entry_count = entry_count;
        
        uint8_t *src = (uint8_t *)mmap;
        for (UINTN i = 0; i < entry_count; i++) {
            EFI_MEMORY_DESCRIPTOR *efi_desc = (EFI_MEMORY_DESCRIPTOR *)src;
            uint64_t base = efi_desc->PhysicalStart;
            uint64_t length = efi_desc->NumberOfPages * 4096;
            uint32_t type = efi_to_db_memtype(efi_desc->Type);

            //check for overlaps with known critical regions
            if (load_info && (base < load_info->phys_end && (base + length) > load_info->phys_base)) {
                type = DB_MEM_KERNEL;
            } else if (initrd_addr && (base < (initrd_addr + initrd_size) && (base + length) > initrd_addr)) {
                type = DB_MEM_INITRD;
            } else if (base < ((uint64_t)boot_info_buffer + sizeof(boot_info_buffer)) && 
                       (base + length) > (uint64_t)boot_info_buffer) {
                type = DB_MEM_BOOTLOADER;
            }

            tag->entries[i].base = base;
            tag->entries[i].length = length;
            tag->entries[i].type = type;
            tag->entries[i].attributes = (uint32_t)efi_desc->Attribute;
            src += desc_size;
        }
        
        tag->size = sizeof(struct db_tag_memory_map) + 
                    entry_count * sizeof(struct db_mmap_entry);
        ptr += DB_ALIGN8(tag->size);
    }
    
    //DB_TAG_CMDLINE
    if ((req_flags & DB_REQ_CMDLINE) && cmdline && cmdline[0]) {
        struct db_tag_cmdline *tag = (struct db_tag_cmdline *)ptr;
        size_t cmdline_len = strlen(cmdline) + 1;
        tag->type = DB_TAG_CMDLINE;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_cmdline) + cmdline_len;
        memcpy(tag->cmdline, cmdline, cmdline_len);
        ptr += DB_ALIGN8(tag->size);
    }
    
    //DB_TAG_EFI_SYSTEM_TABLE
    {
        struct db_tag_efi_system_table *tag = (struct db_tag_efi_system_table *)ptr;
        tag->type = DB_TAG_EFI_SYSTEM_TABLE;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_efi_system_table);
        tag->system_table = (uint64_t)gST;
        ptr += DB_ALIGN8(tag->size);
    }

    //DB_TAG_ACPI_RSDP
    if (req_flags & DB_REQ_ACPI) {
        int is_xsdp = 1;
        EFI_GUID xsdp_guid = ACPI_20_TABLE_GUID;
        void *acpi_table = find_acpi_table(gST, xsdp_guid);
        
        if (!acpi_table) {
            is_xsdp = 0;
            EFI_GUID rsdp_guid = ACPI_TABLE_GUID;
            acpi_table = find_acpi_table(gST, rsdp_guid);
        }

        if (acpi_table) {
            struct db_tag_acpi_rsdp *tag = (struct db_tag_acpi_rsdp *)ptr;
            tag->type = DB_TAG_ACPI_RSDP;
            tag->flags = is_xsdp ? 1 : 0;
            tag->size = sizeof(struct db_tag_acpi_rsdp);
            tag->rsdp_address = (uint64_t)acpi_table;
            ptr += DB_ALIGN8(tag->size);
        }
    }

    //DB_TAG_KERNEL_PHYS
    if (load_info && load_info->phys_base) {
        struct db_tag_kernel_phys *tag = (struct db_tag_kernel_phys *)ptr;
        tag->type = DB_TAG_KERNEL_PHYS;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_kernel_phys);
        tag->phys_base = load_info->phys_base;
        tag->phys_length = load_info->phys_end - load_info->phys_base;
        ptr += DB_ALIGN8(tag->size);
    }

    //DB_TAG_BOOT_TIME
    if (gST->RuntimeServices && gST->RuntimeServices->GetTime) {
        EFI_TIME time;
        if (!EFI_ERROR(gST->RuntimeServices->GetTime(&time, NULL))) {
            struct db_tag_boot_time *tag = (struct db_tag_boot_time *)ptr;
            tag->type = DB_TAG_BOOT_TIME;
            tag->flags = 0;
            tag->size = sizeof(struct db_tag_boot_time);
            
            //simple conversion to Unix timestamp
            uint64_t year = time.Year;
            uint64_t month = time.Month;
            uint64_t day = time.Day;
            uint64_t hour = time.Hour;
            uint64_t minute = time.Minute;
            uint64_t second = time.Second;

            static const uint16_t days_before_month[] = {
                0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
            };

            uint64_t total_days = (year - 1970) * 365;
            total_days += (year - 1969) / 4;
            if (year % 4 == 0 && month <= 2) total_days--;
            total_days += days_before_month[month - 1];
            total_days += day - 1;

            tag->unix_timestamp = total_days * 86400 + hour * 3600 + minute * 60 + second;
            ptr += DB_ALIGN8(tag->size);
        }
    }

    //DB_TAG_KERNEL_FILE
    if (kernel_path) {
        struct db_tag_kernel_file *tag = (struct db_tag_kernel_file *)ptr;
        size_t path_len = strlen(kernel_path) + 1;
        tag->type = DB_TAG_KERNEL_FILE;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_kernel_file) + path_len;
        memcpy(tag->path, kernel_path, path_len);
        ptr += DB_ALIGN8(tag->size);
    }

    //DB_TAG_INITRD
    if (initrd_addr && initrd_size) {
        struct db_tag_initrd *tag = (struct db_tag_initrd *)ptr;
        tag->type = DB_TAG_INITRD;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_initrd);
        tag->start = initrd_addr;
        tag->length = initrd_size;
        ptr += DB_ALIGN8(tag->size);
    }
    
    //DB_TAG_END
    {
        struct db_tag_end *tag = (struct db_tag_end *)ptr;
        tag->type = DB_TAG_END;
        tag->flags = 0;
        tag->size = sizeof(struct db_tag_end);
        ptr += DB_ALIGN8(tag->size);
    }
    
    info->total_size = ptr - boot_info_buffer;
    return info;
}

static EFI_STATUS get_memory_map(
    EFI_MEMORY_DESCRIPTOR **mmap,
    UINTN *mmap_size,
    UINTN *mmap_key,
    UINTN *desc_size,
    UINT32 *desc_version
) {
    EFI_STATUS status;
    
    *mmap_size = 0;
    *mmap = NULL;
    
    status = gBS->GetMemoryMap(mmap_size, *mmap, mmap_key, desc_size, desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) return status;
    
    *mmap_size += 4 * (*desc_size);
    
    status = gBS->AllocatePool(EfiLoaderData, *mmap_size, (VOID **)mmap);
    if (EFI_ERROR(status)) return status;
    
    status = gBS->GetMemoryMap(mmap_size, *mmap, mmap_key, desc_size, desc_version);
    return status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;
    
    //get GOP first
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    
    status = gBS->LocateProtocol(&gop_guid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || !gop) {
        return EFI_UNSUPPORTED;
    }
    
    //initialize graphics
    gfx_init(
        gop->Mode->FrameBufferBase,
        gop->Mode->Info->HorizontalResolution,
        gop->Mode->Info->VerticalResolution,
        gop->Mode->Info->PixelsPerScanLine
    );
    
    //initialize console
    con_init();
    
    //try to load config file
    static Config boot_config;
    void *config_data = NULL;
    uint64_t config_size = 0;
    int have_config = 0;
    
    status = file_load(gImageHandle, gBS, CONFIG_PATH, &config_data, &config_size);
    if (!EFI_ERROR(status) && config_data) {
        if (config_parse((const char *)config_data, config_size, &boot_config) > 0) {
            have_config = 1;
        }
        gBS->FreePool(config_data);
    }
    
    //initialize menu
    menu_init();
    
    if (have_config) {
        //add entries from config
        for (uint32_t i = 0; i < boot_config.entry_count; i++) {
            menu_add_entry(boot_config.entries[i].name, boot_config.entries[i].path);
        }
    }
    
    //show menu and get selection
    int selection = menu_run();
    if (selection < 0) {
        gfx_clear(COLOR_BG);
        con_set_color(COLOR_RED, 0);
        con_print_at(40, 40, "Error: No boot entries found!");
        con_set_color(COLOR_WHITE, 0);
        con_print_at(40, 80, "Please create a config file at:");
        con_print_at(40, 100, "  \\EFI\\BOOT\\delboot.cfg");
        con_set_color(COLOR_GRAY, 0);
        con_print_at(40, 140, "Example config:");
        con_print_at(40, 160, "  [Delta Kernel]");
        con_print_at(40, 180, "  path=\\EFI\\BOOT\\kernel.bin");
        gBS->Stall(5000000);
        return EFI_NOT_FOUND;
    }
    
    //get selected entry info
    MenuEntry *menu_entry = menu_get_entry(selection);
    const char *kernel_path = menu_entry ? menu_entry->path : "\\EFI\\BOOT\\kernel.bin";
    const char *cmdline = "";
    
    if (have_config && (uint32_t)selection < boot_config.entry_count) {
        cmdline = boot_config.entries[selection].cmdline;
    }
    
    //show loading screen
    gfx_clear(COLOR_BG);
    con_set_color(COLOR_WHITE, 0);
    con_print_at(40, 40, "Loading kernel...");
    
    //load kernel
    void *kernel_data = NULL;
    uint64_t kernel_size = 0;
    
    //convert path  (ascii to wchar)
    CHAR16 wpath[256];
    const char *p = kernel_path;
    int i = 0;
    while (*p && i < 255) {
        wpath[i++] = (CHAR16)*p++;
    }
    wpath[i] = 0;
    
    status = file_load(gImageHandle, gBS, wpath, &kernel_data, &kernel_size);
    if (EFI_ERROR(status)) {
        con_set_color(COLOR_RED, 0);
        con_print_at(40, 80, "Failed to load kernel!");
        gBS->Stall(3000000);
        return status;
    }

    //load initrd if specified
    void *initrd_data = NULL;
    uint64_t initrd_size = 0;
    const char *initrd_path = (have_config && (uint32_t)selection < boot_config.entry_count) ? boot_config.entries[selection].initrd : "";
    
    if (initrd_path && initrd_path[0]) {
        con_print_at(40, 60, "Loading initrd...");
        
        CHAR16 winitrd[256];
        const char *p = initrd_path;
        int i = 0;
        while (*p && i < 255) {
            winitrd[i++] = (CHAR16)*p++;
        }
        winitrd[i] = 0;
        
        status = file_load(gImageHandle, gBS, winitrd, &initrd_data, &initrd_size);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_YELLOW, 0); //warning
            con_print_at(40, 80, "Warning: Failed to load initrd! Continuing boot...");
            gBS->Stall(2000000);
            initrd_data = NULL;
            initrd_size = 0;
            //clear status so we don't accidentally return it later
            status = EFI_SUCCESS;
        }
    }
    
    //find DB header
    struct db_request_header *db_req = find_db_header(kernel_data, kernel_size);
    if (db_req == (struct db_request_header *)-1) {
        return EFI_LOAD_ERROR;
    }
    
    uint32_t req_flags = db_req ? db_req->flags : (DB_REQ_FRAMEBUFFER | DB_REQ_MEMORY_MAP);
    
    //add cmdline flag if we have one
    if (cmdline && cmdline[0]) {
        req_flags |= DB_REQ_CMDLINE;
    }
    
    //check if kernel is ELF64 and load it
    uint64_t entry_point = 0;
    elf_load_info_t load_info = {0};
    int is_elf = elf_validate(kernel_data, kernel_size);
    int is_higher_half = 0;
    
    if (is_elf) {
        status = elf_load(gBS, kernel_data, kernel_size, &entry_point, &load_info);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_RED, 0);
            con_print_at(40, 80, "Failed to load ELF segments!");
            gBS->Stall(3000000);
            return status;
        }
        //check if kernel expects higher-half
        if (load_info.virt_base >= KERNEL_VMA) {
            is_higher_half = 1;
        }
        
        //ELF loaded successfully
        con_set_color(COLOR_GRAY, 0);
        con_print_at(40, 100, "Kernel symbols loaded.");
    } else {
        //if raw binary then use DB header entry_point or default to offset 0
        uint32_t entry_offset = db_req ? db_req->entry_point : 0;
        entry_point = (uint64_t)((uint8_t *)kernel_data + entry_offset);
    }
    
    //get memory map
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;
    UINTN mmap_size, mmap_key, desc_size;
    UINT32 desc_version;
    
    status = get_memory_map(&mmap, &mmap_size, &mmap_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        return status;
    }
    
    //set up paging if higher-half kernel
    page_tables_t page_tables = {0};
    if (is_higher_half) {
        status = paging_init(gBS, &page_tables);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_RED, 0);
            con_print_at(40, 80, "Failed to init paging");
            gBS->Stall(3000000);
            return status;
        }
        
        //identity map from UEFI memory map
        status = paging_identity_map(gBS, &page_tables, mmap, mmap_size, desc_size);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_RED, 0);
            con_print_at(40, 80, "Failed identity map");
            gBS->Stall(3000000);
            return status;
        }
        
        //map entire physical memory to HHDM
        status = paging_map_hhdm(gBS, &page_tables, mmap, mmap_size, desc_size);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_RED, 0);
            con_print_at(40, 80, "Failed HHDM map");
            gBS->Stall(3000000);
            return status;
        }

        //map framebuffer explicitly (identity + HHDM)
        uint64_t fb_base = gop->Mode->FrameBufferBase;
        uint64_t fb_size = gop->Mode->FrameBufferSize;
        status = paging_map_framebuffer(gBS, &page_tables, fb_base, fb_size);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_RED, 0);
            con_print_at(40, 80, "Failed framebuffer map");
            gBS->Stall(3000000);
            return status;
        }
        
        //map kernel virt_base -> phys_base (where kernel actually loaded)
        uint64_t kernel_size = load_info.phys_end - load_info.phys_base;
        
        status = paging_map_kernel(gBS, &page_tables, load_info.virt_base, load_info.phys_base, kernel_size);
        if (EFI_ERROR(status)) {
            con_set_color(COLOR_RED, 0);
            con_print_at(40, 80, "Failed kernel map");
            gBS->Stall(3000000);
            return status;
        }
    }
    
    //build boot info
    struct db_boot_info *boot_info = build_boot_info(req_flags, cmdline, gop, mmap, mmap_size, desc_size, &load_info, kernel_path, (uint64_t)initrd_data, initrd_size);
    
    con_print_at(40, 60, "Booting...");
    gBS->Stall(500000);
    
    //get fresh memory map
    gBS->FreePool(mmap);
    status = get_memory_map(&mmap, &mmap_size, &mmap_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        for (;;) __asm__ volatile("hlt");
    }
    
    //set virtual = physical for runtime regions (identity mapping)
    UINTN entry_count = mmap_size / desc_size;
    uint8_t *ptr = (uint8_t *)mmap;
    for (UINTN i = 0; i < entry_count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)ptr;
        desc->VirtualStart = desc->PhysicalStart;
        ptr += desc_size;
    }
    
    //rebuild with fresh map
    boot_info = build_boot_info(req_flags, cmdline, gop, mmap, mmap_size, desc_size, &load_info, kernel_path, (uint64_t)initrd_data, initrd_size);
    
    //exit boot services
    status = gBS->ExitBootServices(gImageHandle, mmap_key);
    if (EFI_ERROR(status)) {
        status = get_memory_map(&mmap, &mmap_size, &mmap_key, &desc_size, &desc_version);
        if (!EFI_ERROR(status)) {
            //redo identity mapping
            ptr = (uint8_t *)mmap;
            entry_count = mmap_size / desc_size;
            for (UINTN i = 0; i < entry_count; i++) {
                EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)ptr;
                desc->VirtualStart = desc->PhysicalStart;
                ptr += desc_size;
            }
            boot_info = build_boot_info(req_flags, cmdline, gop, mmap, mmap_size, desc_size, &load_info, kernel_path, (uint64_t)initrd_data, initrd_size);
            status = gBS->ExitBootServices(gImageHandle, mmap_key);
        }
    }
    
    if (EFI_ERROR(status)) {
        for (;;) __asm__ volatile("hlt");
    }
    
    //call SetVirtualAddressMap for runtime services
    if (gST->RuntimeServices && gST->RuntimeServices->SetVirtualAddressMap) {
        gST->RuntimeServices->SetVirtualAddressMap(mmap_size, desc_size, desc_version, mmap);
    }
    
    con_clear();

    //jump to kernel
    if (is_higher_half) {
        con_clear();

        
        //now jump to kernel, switch to our CR3
        __asm__ volatile("mov %0, %%cr3" : : "r"(page_tables.pml4_phys) : "memory");
        
        __asm__ volatile(
            "mov %0, %%rdi\n\t"  //boot_info to RDI (System V ABI)
            "mov %0, %%rcx\n\t"  //boot_info to RCX (MS ABI)
            "jmp *%1\n\t"
            :
            : "r"(boot_info), "r"(entry_point)
            : "rdi", "rcx", "memory"
        );
    } else {
        //if non-higher-half just jump with identity mapping from UEFI
        __asm__ volatile(
            "mov %1, %%rdi\n\t"
            "mov %1, %%rcx\n\t"
            "jmp *%0\n\t"
            :
            : "r"(entry_point), "r"(boot_info)
            : "rcx", "rdi", "memory"
        );
    }
    
    for (;;) __asm__ volatile("hlt");
}
