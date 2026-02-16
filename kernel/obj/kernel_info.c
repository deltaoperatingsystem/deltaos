#include <obj/object.h>
#include <syscall/syscall.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <arch/timer.h>
#include <drivers/rtc.h>
#include <string.h>
#include <obj/namespace.h>
#include <arch/cpu.h>


//external globals needed for stats
extern uint32 timer_freq;
extern size max_pages;
extern size free_pages;

//convert broken-down time to seconds since 2000-01-01
static uint32 rtc_to_delta_time(rtc_time_t *t) {
    //days per month (non-leap)
    static const uint8 days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    
    uint32 year = t->year;
    uint32 days = 0;
    
    //years since 2000
    for (uint32 y = 2000; y < year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    
    //months
    for (uint8 m = 1; m < t->month; m++) {
        days += days_in_month[m - 1];
        //add leap day for Feb
        if (m == 2) {
            bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
            if (leap) days++;
        }
    }
    
    //days (1-indexed)
    days += t->day - 1;
    
    return days * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
}

static intptr system_get_info(object_t *obj, uint32 topic, void *buf, size len) {
    if (!obj || !buf) return -1;
    
    if (topic == OBJ_INFO_KMEM_STATS) {
        if (len < sizeof(kmem_stats_t)) return -1;
        kmem_stats_t st;
        st.total_ram = (uint64)pmm_get_total_pages() * 4096;
        st.free_ram = (uint64)pmm_get_free_pages() * 4096;
        st.used_ram = st.total_ram - st.free_ram;
        
        //get heap stats
        kheap_stats_t heap;
        kheap_get_stats(&heap);
        st.heap_used = heap.slab_used + heap.large_used;
        st.heap_free = heap.slab_capacity - heap.slab_used;
        
        memcpy(buf, &st, sizeof(st));
        return 0;
    } else if (topic == OBJ_INFO_TIME_STATS) {
        if (len < sizeof(time_stats_t)) return -1;
        time_stats_t st;
        st.ticks = arch_timer_get_ticks();
        
        //calculate uptime in nanoseconds
        if (timer_freq > 0) {
            st.uptime_ns = (st.ticks * 1000000000ULL) / timer_freq;
        } else {
            st.uptime_ns = 0;
        }
        
        //get RTC time as delta-time timestamp
        rtc_time_t rtc;
        rtc_get_time(&rtc);
        st.rtc_time = rtc_to_delta_time(&rtc);
        
        memcpy(buf, &st, sizeof(st));
        return 0;
    } else if (topic == OBJ_INFO_SYSTEM_STATS) {
        if (len < sizeof(system_stats_t)) return -1;
        system_stats_t st;
        memset(&st, 0, sizeof(st));
        
        st.cpu_count = arch_cpu_count();
        st.cpu_freq_mhz = timer_freq / 1000000; //approx from timer
        strncpy(st.os_name, "DeltaOS", sizeof(st.os_name));
        strncpy(st.os_version, "0.1.0", sizeof(st.os_version));
        strncpy(st.arch, "amd64", sizeof(st.arch));
        
        //get CPU vendor via CPUID leaf 0
        uint32 eax, ebx, ecx, edx;
        arch_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
        memcpy(st.cpu_vendor + 0, &ebx, 4);
        memcpy(st.cpu_vendor + 4, &edx, 4);
        memcpy(st.cpu_vendor + 8, &ecx, 4);
        st.cpu_vendor[12] = '\0';
        
        //get CPU brand string via CPUID leaves 0x80000002-4
        uint32 *brand = (uint32 *)st.cpu_brand;
        arch_cpuid(0x80000002, 0, &brand[0], &brand[1], &brand[2], &brand[3]);
        arch_cpuid(0x80000003, 0, &brand[4], &brand[5], &brand[6], &brand[7]);
        arch_cpuid(0x80000004, 0, &brand[8], &brand[9], &brand[10], &brand[11]);
        st.cpu_brand[47] = '\0';
        
        memcpy(buf, &st, sizeof(st));
        return 0;
    }
    
    return -1;
}

static object_ops_t system_ops = {
    .get_info = system_get_info,
    .read = NULL,
    .write = NULL,
    .close = NULL
};

object_t *system_object_create(void) {
    return object_create(OBJECT_SYSTEM, &system_ops, NULL);
}

void kernel_info_init(void) {
    object_t *sys = system_object_create();
    if (sys) {
        ns_register("$devices/system", sys);
        object_deref(sys);
    }
}
