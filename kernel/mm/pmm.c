#include <arch/types.h>
#include <boot/db.h>
#include <lib/io.h>

void pmm_init(void) {
    struct db_tag_memory_map *mmap = db_get_memory_map();
    set_outmode(SERIAL);

    uint64 total_usable = 0;
    struct db_mmap_entry *greatest = {0};
    for (uint32 i = 0; i < mmap->entry_count; i++) {
        struct db_mmap_entry *current = mmap->entries + i;
        if (current->type == DB_MEM_USABLE) {
            printf("Usable memory: 0x%x - 0x%x (0x%x)\n", current->base, current->base + current->length, current->length);
            total_usable += current->length;
            if (current->length > greatest->length) greatest = current;
        }
    }
    printf("Total available memory: 0x%x\n", total_usable);
    printf("Greatest available segment: 0x%x - 0x%x (0x%x)\n", greatest->base, greatest->base + greatest->length, greatest->length);
}