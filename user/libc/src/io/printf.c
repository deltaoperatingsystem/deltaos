#include <io.h>
#include <types.h>
#include <args.h>
#include <system.h>

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    //use a local buffer to aggregate output and minimize syscalls
    //this also ensures atomicity in the terminal due to kernel-side console lock
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    
    if (len > 0) {
        if (__stdout != INVALID_HANDLE) {
            //cap at buffer size for the write
            size to_write = (len < (int)sizeof(buf)) ? (size)len : (sizeof(buf) - 1);
            handle_write(__stdout, buf, to_write);
        }
    }

    va_end(args);
}