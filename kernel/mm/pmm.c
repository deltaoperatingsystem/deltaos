#include <mm/pmm.h>
#include <mm/mm.h>
#include <arch/cpu.h>
#include <boot/db.h>
#include <drivers/serial.h>

static uint8 *bitmap = NULL;
static size bitmap_size = 0; //in bytes
size max_pages = 0;

#define BITMAP_SET(bit)   (bitmap[(bit) / 8] |= (1 << ((bit) % 8)))
#define BITMAP_CLEAR(bit) (bitmap[(bit) / 8] &= ~(1 << ((bit) % 8)))
#define BITMAP_TEST(bit)  (bitmap[(bit) / 8] & (1 << ((bit) % 8)))

static size last_free_page = 0;
size free_pages = 0;

void pmm_init(void) {
    struct db_tag_memory_map *mmap = db_get_memory_map();
    if (!mmap) {
        serial_write("[pmm] ERROR: no memory map tag found\n");
        return;
    }

    uint64 max_addr = 0;
    uintptr entries_ptr = (uintptr)mmap->entries;

    serial_write("[pmm] memory map:\n");
    for (uint32 i = 0; i < mmap->entry_count; i++) {
        struct db_mmap_entry *current = (struct db_mmap_entry *)(entries_ptr + i * mmap->entry_size);
        if (current->type == DB_MEM_USABLE) {
            serial_write("  - ");
            serial_write_hex(current->base);
            serial_write(" - ");
            serial_write_hex(current->base + current->length);
            serial_write(" (usable)\n");

            if (current->base + current->length > max_addr) {
                max_addr = current->base + current->length;
            }
        }
    }

    if (max_addr == 0) {
        serial_write("[pmm] ERROR: no usable memory regions found in map\n");
        return;
    }

    max_pages = max_addr / PAGE_SIZE;
    bitmap_size = max_pages / 8;
    if (max_pages % 8) bitmap_size++;

    serial_write("[pmm] max_addr: ");
    serial_write_hex(max_addr);
    serial_write(", bitmap_size: ");
    serial_write_hex(bitmap_size);
    serial_write(" bytes\n");

    //find a place for the bitmap (avoiding the first 1MB if possible)
    bool found = false;
    for (uint32 i = 0; i < mmap->entry_count; i++) {
        struct db_mmap_entry *current = (struct db_mmap_entry *)(entries_ptr + i * mmap->entry_size);
        if (current->type == DB_MEM_USABLE && current->length >= bitmap_size) {
            //don't put bitmap at address 0 try to keep it above 1MB
            if (current->base >= 0x100000) {
                //use HHDM for bitmap address
                bitmap = (uint8 *)P2V(current->base);
                found = true;
                break;
            }
        }
    }

    //fallback if no region above 1MB is large enough
    if (!found) {
        for (uint32 i = 0; i < mmap->entry_count; i++) {
            struct db_mmap_entry *current = (struct db_mmap_entry *)(entries_ptr + i * mmap->entry_size);
            if (current->type == DB_MEM_USABLE && current->length >= bitmap_size && current->base > 0) {
                bitmap = (uint8 *)P2V(current->base);
                found = true;
                break;
            }
        }
    }

    if (!found) {
        serial_write("[pmm] ERROR: could not find safe location for bitmap\n");
        return;
    }

    //initially mark everything as reserved (1)
    for (size i = 0; i < bitmap_size; i++) bitmap[i] = 0xFF;

    //mark usable regions as free (0)
    for (uint32 i = 0; i < mmap->entry_count; i++) {
        struct db_mmap_entry *current = (struct db_mmap_entry *)(entries_ptr + i * mmap->entry_size);
        if (current->type == DB_MEM_USABLE) {
            size start_page = current->base / PAGE_SIZE;
            size page_count = current->length / PAGE_SIZE;
            for (size j = 0; j < page_count; j++) {
                if (start_page + j < max_pages) {
                    if (BITMAP_TEST(start_page + j)) {
                        BITMAP_CLEAR(start_page + j);
                        free_pages++;
                    }
                }
            }
        }
    }
    
    //reserve the bitmap itself
    uintptr bitmap_phys = V2P(bitmap);
    size bitmap_start_page = bitmap_phys / PAGE_SIZE;
    size bitmap_page_count = bitmap_size / PAGE_SIZE;
    if (bitmap_size % PAGE_SIZE) bitmap_page_count++;
    for (size i = 0; i < bitmap_page_count; i++) {
        if (!BITMAP_TEST(bitmap_start_page + i)) {
            BITMAP_SET(bitmap_start_page + i);
            free_pages--;
        }
    }

    //reserve the kernel physical segments
    struct db_tag_kernel_phys *kphys = db_get_kernel_phys();
    if (kphys) {
        size k_start = kphys->phys_base / PAGE_SIZE;
        size k_count = kphys->phys_length / PAGE_SIZE;
        if (kphys->phys_length % PAGE_SIZE) k_count++;
        for (size i = 0; i < k_count; i++) {
            if (k_start + i < max_pages) {
                if (!BITMAP_TEST(k_start + i)) {
                    BITMAP_SET(k_start + i);
                    free_pages--;
                }
            }
        }
    }

    //reserve the boot info structure and all tags (the tags region)
    struct db_boot_info *info = db_get_boot_info();
    if (info) {
        size info_start = (uintptr)V2P(info) / PAGE_SIZE;
        size info_count = info->total_size / PAGE_SIZE;
        if (info->total_size % PAGE_SIZE) info_count++;
        for (size i = 0; i < info_count; i++) {
            if (info_start + i < max_pages) {
                if (!BITMAP_TEST(info_start + i)) {
                    BITMAP_SET(info_start + i);
                    free_pages--;
                }
            }
        }
    }
    
    //reserve Initrd
    struct db_tag_initrd *initrd = db_get_initrd();
    if (initrd) {
        size rd_start = initrd->start / PAGE_SIZE;
        size rd_count = initrd->length / PAGE_SIZE;
        if (initrd->length % PAGE_SIZE) rd_count++;
        for (size i = 0; i < rd_count; i++) {
            if (rd_start + i < max_pages) {
                if (!BITMAP_TEST(rd_start + i)) {
                    BITMAP_SET(rd_start + i);
                    free_pages--;
                }
            }
        }
    }

    //reserve page 0
    if (!BITMAP_TEST(0)) {
        BITMAP_SET(0);
        free_pages--;
    }

    serial_write("[pmm] initialized, bitmap @ ");
    serial_write_hex((uintptr)bitmap);
    serial_write("\n");
}

void *pmm_alloc(size pages) {
    if (pages == 0) return NULL;

    irq_state_t flags = arch_irq_save();

    size consecutive = 0;
    size start_bit = 0;

    //word skipping optimization
    uword *bitmap_words = (uword *)bitmap;
    size max_words = max_pages / ARCH_BITS;

    for (size i = last_free_page; i < max_pages; ) {
        //if we are at the start of a word and need more than current bit try skipping
        if ((i % ARCH_BITS == 0) && (consecutive == 0)) {
            while (i / ARCH_BITS < max_words && bitmap_words[i / ARCH_BITS] == (uword)-1) {
                i += ARCH_BITS;
            }
            if (i >= max_pages) break;
        }

        if (!BITMAP_TEST(i)) {
            if (consecutive == 0) start_bit = i;
            consecutive++;
            if (consecutive == pages) {
                for (size j = 0; j < pages; j++) {
                    BITMAP_SET(start_bit + j);
                }
                free_pages -= pages;
                last_free_page = start_bit + pages;
                void *res = (void *)(uintptr)(start_bit * PAGE_SIZE);
                arch_irq_restore(flags);
                return res;
            }
        } else {
            consecutive = 0;
        }
        i++;
    }

    //if we reached the end try searching from the beginning once
    if (last_free_page > 0) {
        size search_limit = last_free_page;
        last_free_page = 0;
        consecutive = 0;
        for (size i = 0; i < search_limit; ) {
            if ((i % ARCH_BITS == 0) && (consecutive == 0)) {
                while (i / ARCH_BITS < max_words && i + ARCH_BITS <= search_limit && bitmap_words[i / ARCH_BITS] == (uword)-1) {
                    i += ARCH_BITS;
                }
                if (i >= search_limit) break;
            }

            if (!BITMAP_TEST(i)) {
                if (consecutive == 0) start_bit = i;
                consecutive++;
                if (consecutive == pages) {
                    for (size j = 0; j < pages; j++) {
                        BITMAP_SET(start_bit + j);
                    }
                    free_pages -= pages;
                    last_free_page = start_bit + pages;
                    void *res = (void *)(uintptr)(start_bit * PAGE_SIZE);
                    arch_irq_restore(flags);
                    return res;
                }
            } else {
                consecutive = 0;
            }
            i++;
        }
    }

    arch_irq_restore(flags);
    return NULL;
}

void pmm_free(void *ptr, size pages) {
    if (!ptr) return;

    irq_state_t flags = arch_irq_save();
    uintptr addr = (uintptr)ptr;
    size start_bit = addr / PAGE_SIZE;

    for (size i = 0; i < pages; i++) {
        if (start_bit + i < max_pages) {
            if (!BITMAP_TEST(start_bit + i)) continue;
            BITMAP_CLEAR(start_bit + i);
            free_pages++;
        }
    }

    if (start_bit < last_free_page) {
        last_free_page = start_bit;
    }

    arch_irq_restore(flags);
}