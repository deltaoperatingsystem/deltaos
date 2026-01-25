#include <system.h>
#include <io.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        puts("Usage: sectorwrite <device> <lba> <pattern>\n");
        puts("Example: sectorwrite $devices/disks/nvme0n1p1 100 DEADBEEF\n");
        puts("Writes pattern repeatedly to fill sector\n");
        return 1;
    }
    
    char *device = argv[1];
    uint64 lba = 0;
    char *pattern = argv[3];
    
    //parse LBA from argument
    for (char *p = argv[2]; *p; p++) {
        lba = lba * 10 + (*p - '0');
    }
    
    handle_t h = get_obj(INVALID_HANDLE, device, RIGHT_READ | RIGHT_WRITE);
    if (h == INVALID_HANDLE) {
        printf("sectorwrite: cannot open '%s'\n", device);
        return 1;
    }
    
    //seek to LBA (assuming 512 byte sectors)
    uint64 offset = lba * 512;
    if (handle_seek(h, offset, HANDLE_SEEK_SET) < 0) {
        printf("sectorwrite: seek to LBA %llu failed\n", lba);
        handle_close(h);
        return 1;
    }
    
    //fill buffer with pattern
    char buf[512];
    size plen = strlen(pattern);
    for (int i = 0; i < 512; i++) {
        buf[i] = pattern[i % plen];
    }
    
    //write one sector
    int len = handle_write(h, buf, 512);
    if (len < 0) {
        printf("sectorwrite: write failed (error %d)\n", len);
        handle_close(h);
        return 1;
    }
    
    printf("Wrote %d bytes to LBA %llu with pattern '%s'\n", len, lba, pattern);
    
    handle_close(h);
    return 0;
}
