#include <io.h>

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putc(' ');
        puts(argv[i]);
    }
    putc('\n');
    return 0;
}
