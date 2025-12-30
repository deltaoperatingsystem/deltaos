#include <system.h>
#include <io.h>

static void print_int(int n) {
    char buf[16];
    int i = 0;
    
    if (n == 0) {
        puts("0");
        return;
    }
    
    char tmp[16];
    int j = 0;
    while (n > 0) {
        tmp[j++] = '0' + (n % 10);
        n /= 10;
    }
    while (j > 0) {
        buf[i++] = tmp[--j];
    }
    buf[i] = '\0';
    puts(buf);
}

int main(int argc, char *argv[]) {
    puts("[user] hello from userspace!\n");

    puts("[user] argc = ");
    print_int(argc);
    puts("\n");

    for (int a = 0; a < argc; a++) {
        puts("[user] argv[");
        print_int(a);
        puts("] = ");
        if (argv[a]) {
            puts(argv[a]);
        } else {
            puts("(null)");
        }
        puts("\n");
    }
    
    puts("[user] getpid() = ");
    print_int((int)getpid());
    puts("\n");
    
    puts("[user] syscall test complete, exiting\n");
    
    return 0;
}
