#include <arch/types.h>
#include <arch/interrupts.h>
#include <arch/timer.h>
#include <boot/db.h>
#include <lib/io.h>
#include <lib/string.h>
#include <drivers/serial.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <mm/kheap.h>
#include <obj/handle.h>
#include <proc/process.h>
#include <drivers/pci.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/acpi/acpi.h>
#include <arch/amd64/smp/smp.h>
#include <arch/percpu.h>

extern void kernel_main(const char *cmdline);
extern void enable_sse(void);

void arch_init(struct db_boot_info *boot_info) {
    //early console for debugging
    serial_init();
    io_enable_serial();
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
        
        //parse early command-line arguments that affect initialization
        //check for `noapic` flag to force PIC mode
        char cmdline_buf[256];
        size len = strlen(cmdline);
        if (len >= sizeof(cmdline_buf)) len = sizeof(cmdline_buf) - 1;
        memcpy(cmdline_buf, cmdline, len);
        cmdline_buf[len] = '\0';
        
        char *arg = strtok(cmdline_buf, " ");
        while (arg) {
            if (strcmp(arg, "noapic") == 0) {
                apic_set_force_pic(true);
                printf("[amd64] PIC mode forced via command line\n");
            }
            arg = strtok(NULL, " ");
        }
    }

    pmm_init();
    vmm_init();
    kheap_init();
    handle_init();
    acpi_init();
    
    //initialize per-CPU data early
    percpu_init();
    
    proc_init();
    
    enable_sse();
    puts("[amd64] SSE enabled\n");
    
    //set up interrupt infrastructure
    arch_interrupts_init();
    puts("[amd64] interrupts initialized\n");
    
    //enable interrupts
    arch_interrupts_enable();
    puts("[amd64] interrupts enabled\n");
    
    //initialize timer at 1000Hz
    arch_timer_init(1000);
    puts("[amd64] timer initialized @ 1000Hz\n");
    
    pci_init();
    
    //initialize SMP (start APs)
    smp_init();
    
    //jump to MI kernel
    puts("[amd64] jumping to kernel_main\n\n");
    kernel_main(cmdline);
}
