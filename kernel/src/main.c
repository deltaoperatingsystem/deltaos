#include <arch/types.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/rtc.h>
#include <drivers/serial.h>
#include <lib/string.h>
#include <lib/io.h>
#include <lib/path.h>
#include <boot/db.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>
#include <obj/handle.h>
#include <obj/namespace.h>
#include <fs/tmpfs.h>
#include <fs/fs.h>
#include <fs/initrd.h>
#include <proc/sched.h>
#include <string.h>

extern void shell(void);

void kernel_main(void) {
    set_outmode(SERIAL);
    puts("kernel_main started\n");
    
    fb_init();
    fb_init_backbuffer();
    serial_init_object();
    keyboard_init();
    rtc_init();
    tmpfs_init();
    initrd_init();
    sched_init();
    
    if (fb_available()) {
        con_init();
        con_clear();
        set_outmode(CONSOLE);
        
        con_set_fg(FB_RGB(0, 255, 128));
        puts("DeltaOS Kernel\n");
        puts("==============\n\n");
        
        con_set_fg(FB_WHITE);
        puts("Console: initialized\n");
        printf("Timer: running @ %dHz\n", arch_timer_getfreq());

        struct db_tag_memory_map *mmap = db_get_memory_map();
        if (mmap) {
            int usable = 0, kernel = 0, boot = 0;
            for (uint32 i = 0; i < mmap->entry_count; i++) {
                if (mmap->entries[i].type == DB_MEM_USABLE) usable++;
                else if (mmap->entries[i].type == DB_MEM_KERNEL) kernel++;
                else if (mmap->entries[i].type == DB_MEM_BOOTLOADER) boot++;
            }
            printf("Memory: %d usable, %d kernel, %d bootloader regions\n", usable, kernel, boot);
        }
        
        struct db_tag_acpi_rsdp *acpi = db_get_acpi_rsdp();
        if (acpi) {
            printf("ACPI: RSDP found at 0x%lX (%s)\n", acpi->rsdp_address, acpi->flags & 1 ? "XSDP" : "RSDP");
        } else {
            puts("ACPI: RSDP not found\n");
        }
    
        
        //test object system
        handle_t h = handle_open("$devices/console", 0);
        if (h != INVALID_HANDLE) {
            handle_write(h, "Object system: working!\n", 24);
            handle_close(h);
        }

    }
    
    shell();    
}
