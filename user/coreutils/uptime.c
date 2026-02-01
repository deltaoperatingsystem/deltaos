#include <system.h>
#include <io.h>

typedef struct {
    uint32 timer_hz;
    uint32 reserved;
} kernel_info_timer_t;

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    uint32 hz = 1000; //fallback
    handle_t h = get_obj(INVALID_HANDLE, "$kernel/timer", RIGHT_READ);
    if (h != INVALID_HANDLE) {
        kernel_info_timer_t info;
        if (handle_read(h, &info, sizeof(info)) == sizeof(info)) {
            hz = info.timer_hz;
        }
        handle_close(h);
    }

    uint64 ticks = get_ticks();
    uint64 seconds = ticks / hz;
    
    uint64 hours = seconds / 3600;
    seconds %= 3600;
    uint64 minutes = seconds / 60;
    seconds %= 60;
    
    printf("up %u:%02u:%02u\n", (uint32)hours, (uint32)minutes, (uint32)seconds);
    
    return 0;
}
