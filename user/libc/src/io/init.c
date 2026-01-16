#include <io.h>
#include <system.h>
#include <string.h>

//global stdout handle
int32 __stdout = INVALID_HANDLE;
int32 __stdin = INVALID_HANDLE;

void _io_init(void) {
    //get console handle from devices namespace
    __stdout = get_obj(INVALID_HANDLE, "$devices/vt0", RIGHT_WRITE);
    __stdin = get_obj(INVALID_HANDLE, "$devices/keyboard", RIGHT_READ);
}