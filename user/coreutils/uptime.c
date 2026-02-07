#include <system.h>
#include <io.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    handle_t h = get_obj(INVALID_HANDLE, "$devices/system", RIGHT_READ);
    if (h == INVALID_HANDLE) {
        printf("uptime: failed to open system object\n");
        return 1;
    }

    time_stats_t ts;
    uint64 seconds;
    
    if (object_get_info(h, OBJ_INFO_TIME_STATS, &ts, sizeof(ts)) == 0) {
         seconds = ts.uptime_ns / 1000000000ULL;
    } else {
        printf("uptime: failed to get time stats\n");
        handle_close(h);
        return 1;
    }
    handle_close(h);
    
    uint64 hours = seconds / 3600;
    seconds %= 3600;
    uint64 minutes = seconds / 60;
    seconds %= 60;
    
    printf("up %u:%02u:%02u\n", (uint32)hours, (uint32)minutes, (uint32)seconds);
    
    return 0;
}
