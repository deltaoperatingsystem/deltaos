#include <system.h>
#include <io.h>
#include <string.h>

int main(int argc, char *argv[]) {
    //if no path provided use "." (current directory)
    const char *path = (argc > 1) ? argv[1] : ".";
    
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
            puts(entries[i].name);
            puts("\n");
        }
    }
    
    handle_close(dir);
    return 0;
}
