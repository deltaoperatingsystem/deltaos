#include <system.h>
#include <io.h>
#include <string.h>

static void format_size(uint64 bytes, char *buf) {
    const char *units[] = {"B", "K", "M", "G", "T"};
    int unit = 0;
    double size = (double)bytes;

    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }

    //very basic float to string
    int integral = (int)size;
    int fractional = (int)((size - integral) * 10);
    snprintf(buf, 16, "%d.%d%s", integral, fractional, units[unit]);
}

int main(int argc, char *argv[]) {
    const char *path = "$devices/disks";
    
    handle_t dir = get_obj(INVALID_HANDLE, path, RIGHT_READ);
    if (dir == INVALID_HANDLE) {
        printf("lsblk: cannot access '%s'\n", path);
        return 1;
    }
    
    printf("%-16s %-10s %-10s\n", "NAME", "SIZE", "SECTORS");
    
    dirent_t entries[16];
    uint32 index = 0;
    
    while (1) {
        int n = readdir(dir, entries, 16, &index);
        if (n <= 0) break;
        
        for (int i = 0; i < n; i++) {
            char full_path[128];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entries[i].name);
            
            stat_t st;
            if (stat(full_path, &st) == 0) {
                char size_str[16];
                format_size(st.size, size_str);
                printf("%-16s %-10s %-10llu\n", entries[i].name, size_str, st.size / 512);
            } else {
                printf("%-16s %-10s %-10s\n", entries[i].name, "???", "???");
            }
        }
    }
    
    handle_close(dir);
    return 0;
}
