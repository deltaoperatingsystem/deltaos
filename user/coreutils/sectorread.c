#include <system.h>
#include <io.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        puts("Usage: sectorread <device> <lba>\n");
        puts("Example: sectorread $devices/disks/nvme0n1 0\n");
        return 1;
    }
    
    char *device = argv[1];
    uint64 lba = 0;
    
    //parse LBA from argument
    for (char *p = argv[2]; *p; p++) {
        lba = lba * 10 + (*p - '0');
    }
    
    handle_t h = get_obj(INVALID_HANDLE, device, RIGHT_READ);
    if (h == INVALID_HANDLE) {
        printf("sectorread: cannot open '%s'\n", device);
        return 1;
    }
    
    //seek to LBA (assuming 512 byte sectors)
    uint64 offset = lba * 512;
    if (handle_seek(h, offset, HANDLE_SEEK_SET) < 0) {
        printf("sectorread: seek to LBA %llu failed\n", lba);
        handle_close(h);
        return 1;
    }
    
    //read one sector
    char buf[512];
    int len = handle_read(h, buf, 512);
    if (len < 0) {
        printf("sectorread: read failed (error %d)\n", len);
        handle_close(h);
        return 1;
    }
    
    printf("Read %d bytes from LBA %llu:\n", len, lba);
    
    //hex dump
    for (int i = 0; i < len; i += 16) {
        printf("%04X: ", i);
        for (int j = 0; j < 16 && (i + j) < len; j++) {
            printf("%02X ", (unsigned char)buf[i + j]);
        }
        puts("  ");
        for (int j = 0; j < 16 && (i + j) < len; j++) {
            char c = buf[i + j];
            putc((c >= 32 && c < 127) ? c : '.');
        }
        puts("\n");
    }
    
    handle_close(h);
    return 0;
}
