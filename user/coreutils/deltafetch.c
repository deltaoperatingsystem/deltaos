#include <system.h>
#include <io.h>
#include <string.h>

static void format_mem(char *buf, uint64 bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, 16, "%llu GB", bytes / (1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, 16, "%llu MB", bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, 16, "%llu KB", bytes / 1024);
    } else {
        snprintf(buf, 16, "%llu B", bytes);
    }
}

static void format_uptime(char *buf, uint64 uptime_ns) {
    uint64 secs = uptime_ns / 1000000000ULL;
    uint64 mins = secs / 60;
    uint64 hours = mins / 60;
    uint64 days = hours / 24;
    
    if (days > 0) {
        snprintf(buf, 32, "%llud %lluh %llum", days, hours % 24, mins % 60);
    } else if (hours > 0) {
        snprintf(buf, 32, "%lluh %llum %llus", hours, mins % 60, secs % 60);
    } else if (mins > 0) {
        snprintf(buf, 32, "%llum %llus", mins, secs % 60);
    } else {
        snprintf(buf, 32, "%llus", secs);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    handle_t sys = get_obj(INVALID_HANDLE, "$devices/system", RIGHT_GET_INFO);
    if (sys == INVALID_HANDLE) {
        printf("deltafetch: cannot open system object\n");
        return 1;
    }
    
    system_stats_t sysinfo;
    kmem_stats_t meminfo;
    time_stats_t timeinfo;
    
    if (object_get_info(sys, OBJ_INFO_SYSTEM_STATS, &sysinfo, sizeof(sysinfo)) < 0) {
        printf("deltafetch: failed to get system info\n");
        handle_close(sys);
        return 1;
    }
    
    if (object_get_info(sys, OBJ_INFO_KMEM_STATS, &meminfo, sizeof(meminfo)) < 0) {
        printf("deltafetch: failed to get memory info\n");
        handle_close(sys);
        return 1;
    }
    
    if (object_get_info(sys, OBJ_INFO_TIME_STATS, &timeinfo, sizeof(timeinfo)) < 0) {
        printf("deltafetch: failed to get time info\n");
        handle_close(sys);
        return 1;
    }
    
    handle_close(sys);
    
    //format values
    char total_mem[16], used_mem[16], uptime_str[32];
    format_mem(total_mem, meminfo.total_ram);
    format_mem(used_mem, meminfo.used_ram);
    format_uptime(uptime_str, timeinfo.uptime_ns);
    
    //print info
    printf("\n");
    printf("   OS:      %s %s\n", sysinfo.os_name, sysinfo.os_version);
    printf("   Arch:    %s\n", sysinfo.arch);
    printf("   CPU:     %s\n", sysinfo.cpu_brand);
    printf("   Cores:   %u\n", sysinfo.cpu_count);
    printf("   Memory:  %s / %s\n", used_mem, total_mem);
    printf("   Uptime:  %s\n", uptime_str);
    printf("\n");
    
    return 0;
}
