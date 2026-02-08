#include <sys/stat.h>
#include <io.h>

int main(int argc, char**argv) {
    if (argc != 2) {
        puts("Usage: mkdir <path>\n");
        return -1;
    }
    int res = mkdir(argv[1]);
    if (res < 0) {
        printf("Failed to create path '%s' (error %d)\n", argv[1], res);
        return res;
    }
}
