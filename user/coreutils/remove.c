#include <isoc/stdio.h>
#include <io.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("Usage: remove <path> [path2 ...]\n");
        return 1;
    }

    int success = 0;
    for (int i = 1; i < argc; i++) {
        int res = remove(argv[i]);
        if (res < 0) {
            printf("remove: Failed to remove '%s' (error %d)\n", argv[i], res);
        } else {
            success++;
        }
    }

    return (success == argc - 1) ? 0 : 1;
}
