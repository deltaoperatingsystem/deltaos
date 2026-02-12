#include <fs.h>
#include <io.h>

int main(int argc, char**argv) {
    if (argc != 2) {
        puts("Usage: rm <path>\n");
        return -1;
    }
    int res = remove(argv[1]);
    if (res < 0) {
        printf("Failed to remove file '%s' (error %d)\n", argv[1], res);
        return res;
    }

    return 0;
}
