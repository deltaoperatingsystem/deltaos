#include <system.h>
#include <io.h>
#include <string.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    handle_t log = get_obj(INVALID_HANDLE, "$kernel/log", RIGHT_READ);
    if (log == INVALID_HANDLE) {
        puts("klog: cannot access $kernel/log\n");
        return 1;
    }
    
    //get size
    stat_t st;
    if (fstat(log, &st) != 0) {
        puts("klog: cannot stat $kernel/log\n");
        handle_close(log);
        return 1;
    }
    
    if (st.size == 0) {
        puts("(kernel log empty)\n");
        handle_close(log);
        return 0;
    }
    
    //read and print in chunks
    char buf[512];
    
    while (1) {
        int n = handle_read(log, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        
        buf[n] = '\0';
        puts(buf);
    }
    
    handle_close(log);
    return 0;
}
