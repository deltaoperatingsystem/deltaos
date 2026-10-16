#include <isoc/stdio.h>
#include <io.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        puts("Usage: rename <oldpath> <newpath>");
        return 1;
    }

    int res = rename(argv[1], argv[2]);
    if (res < 0) {
        printf("rename: Failed to rename '%s' to '%s' (error %d)\n", argv[1], argv[2], res);
        return 1;
    }

    return 0;
}
