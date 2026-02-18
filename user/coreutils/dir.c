#include <system.h>
#include <io.h>
#include <string.h>

static void format_size(char *buf, size sz) {
    if (sz >= 1024 * 1024) {
        snprintf(buf, 16, "%llu.%lluM", sz / (1024*1024), (sz % (1024*1024)) / 102400);
    } else if (sz >= 1024) {
        snprintf(buf, 16, "%llu.%lluK", sz / 1024, (sz % 1024) / 102);
    } else {
        snprintf(buf, 16, "%llu", sz);
    }
}

static void format_time(char *buf, uint64 delta_time) {
    //delta_time is seconds since 2000-01-01
    static const uint8 days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    
    uint32 secs = delta_time % 60;
    uint32 mins = (delta_time / 60) % 60;
    uint32 hours = (delta_time / 3600) % 24;
    uint32 days = delta_time / 86400;
    
    //convert days to year/month/day
    uint32 year = 2000;
    while (1) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32 year_days = leap ? 366 : 365;
        if (days < year_days) break;
        days -= year_days;
        year++;
    }
    
    uint32 month = 1;
    while (month <= 12) {
        uint32 mdays = days_in_month[month - 1];
        if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) mdays++;
        if (days < mdays) break;
        days -= mdays;
        month++;
    }
    uint32 day = days + 1;
    
    snprintf(buf, 24, "%04u-%02u-%02u %02u:%02u", year, month, day, hours, mins);
    (void)secs;
}

int main(int argc, char *argv[]) {
    bool long_format = false;
    const char *path = ".";
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_format = true;
        } else {
            path = argv[i];
        }
    }
    
    handle_t dir = get_obj(INVALID_HANDLE, path, RIGHT_READ);
    if (dir == INVALID_HANDLE) {
        printf("ls: cannot access '%s'\n", path);
        return 1;
    }
    
    dirent_t entries[16];
    uint32 index = 0;
    
    while (1) {
        int n = readdir(dir, entries, 16, &index);
        if (n <= 0) break;
        
        for (int i = 0; i < n; i++) {
            if (long_format) {
                //get stat info
                char full_path[256];
                if (strcmp(path, ".") == 0) {
                    snprintf(full_path, sizeof(full_path), "%s", entries[i].name);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", path, entries[i].name);
                }
                
                stat_t st;
                if (stat(full_path, &st) == 0) {
                    char type = (st.type == 2) ? 'd' : '-';
                    char size_buf[16];
                    char time_buf[24];
                    format_size(size_buf, st.size);
                    format_time(time_buf, st.ctime);
                    printf("%c %8s %s %s\n", type, size_buf, time_buf, entries[i].name);
                } else {
                    printf("? %8s %s %s\n", "?", "????-??-?? ??:??", entries[i].name);
                }
            } else {
                puts(entries[i].name);
                puts("\n");
            }
        }
    }
    
    handle_close(dir);
    return 0;
}
