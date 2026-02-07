#include <syscall/syscall.h>
#include <lib/io.h>
#include <arch/timer.h>

intptr sys_debug_write(const char *buf, size count) {
    debug_write(buf, count);
    return (intptr)count;
}

intptr sys_get_ticks(void) {
    return (intptr)arch_timer_get_ticks();
}

intptr sys_object_get_info(handle_t h, uint32 topic, void *ptr, size len) {
    if (!handle_has_rights(h, HANDLE_RIGHT_GET_INFO)) return -1;
    
    object_t *obj = handle_get(h);
    if (!obj) return -1;
    
    intptr ret = object_get_info(obj, topic, ptr, len);
    return ret;
}
