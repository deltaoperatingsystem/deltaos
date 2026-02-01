#include <obj/kernel_info.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <lib/io.h>
#include <string.h>

//timer hz 
extern uint32 timer_freq;
extern size max_pages;
extern size free_pages;

static ssize info_obj_read(object_t *obj, void *buf, size len, size offset) {
    uint32 category = (uintptr)obj->data;
    
    if (category == KERNEL_INFO_TIMER) {
        kernel_info_timer_t info;
        info.timer_hz = timer_freq;
        info.reserved = 0;
        
        if (offset >= sizeof(info)) return 0;
        if (offset + len > sizeof(info)) len = sizeof(info) - offset;
        
        memcpy(buf, (uint8*)&info + offset, len);
        return len;
    } else if (category == KERNEL_INFO_MEM) {
        kernel_info_mem_t info;
        info.total_pages = max_pages;
        info.free_pages = free_pages;
        
        if (offset >= sizeof(info)) return 0;
        if (offset + len > sizeof(info)) len = sizeof(info) - offset;
        
        memcpy(buf, (uint8*)&info + offset, len);
        return len;
    }
    
    return -1;
}

static object_ops_t info_ops = {
    .read = info_obj_read,
    .write = NULL,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL,
    .stat = NULL
};

static void register_info(const char *name, uint32 category) {
    object_t *obj = object_create(OBJECT_INFO, &info_ops, (void*)(uintptr)category);
    if (obj) {
        char path[64];
        snprintf(path, sizeof(path), "$kernel/%s", name);
        ns_register(path, obj);
        object_deref(obj);
    }
}

void kernel_info_init(void) {
    //klog_init dependency - this must create $kernel before klog_init runs
    object_t *kernel_dir = ns_create_dir("$kernel/");
    if (kernel_dir) {
        ns_register("$kernel", kernel_dir);
        object_deref(kernel_dir);
    }
    
    register_info("timer", KERNEL_INFO_TIMER);
    register_info("mem", KERNEL_INFO_MEM);
}
