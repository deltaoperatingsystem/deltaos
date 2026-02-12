#include <drivers/init.h>
#include <lib/io.h>
#include <obj/klog.h>
#include <obj/kernel_info.h>

extern driver_init_func_t __driver_init_start[];
extern driver_init_func_t __driver_init_end[];

void init_drivers(void) {
    printf("[drivers] starting driver initialization...\n");
    
    for (driver_init_func_t *init = __driver_init_start; init < __driver_init_end; init++) {
        if (*init) {
            (*init)();
        }
    }
    
    //core services that depend on drivers being ready
    kernel_info_init();
    klog_init();
    
    printf("[drivers] driver initialization complete\n");
}