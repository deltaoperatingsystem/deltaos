#include <arch/types.h>
#include <arch/interrupts.h>
#include <arch/timer.h>
#include <boot/db.h>
#include <lib/io.h>
#include <drivers/serial.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>
#include <obj/handle.h>
#include <proc/process.h>

extern void kernel_main(void);

void arch_init(struct db_boot_info *boot_info) {
    //early console for debugging
    serial_init();
    set_outmode(SERIAL);

    puts("\x1b[2J\x1b[H");
    puts("[amd64] initializing...\n");
    
    //convert physical boot_info from bootloader to virtual via HHDM
    boot_info = (struct db_boot_info *)P2V(boot_info);

    //parse boot info from bootloader
    db_parse(boot_info);

    const char *cmdline = db_get_cmdline();
    if (cmdline) {
        printf("[amd64] cmdline = '%s'\n", cmdline);
    }

    pmm_init();
    vmm_init();
    kheap_init();
    handle_init();
    proc_init();
    
    //set up interrupt infrastructure
    arch_interrupts_init();
    puts("[amd64] interrupts initialized\n");
    
    //enable interrupts
    arch_interrupts_enable();
    puts("[amd64] interrupts enabled\n");
    
    //initialize timer at 100Hz
    arch_timer_init(100);
    puts("[amd64] timer initialized @ 100Hz\n");
    
    //jump to MI kernel
    puts("[amd64] jumping to kernel_main\n\n");
    kernel_main();
}
