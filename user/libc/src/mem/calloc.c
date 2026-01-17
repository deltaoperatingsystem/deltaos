#include <mem.h>
#include <string.h>

void *calloc(size nmemb, size element_size) {
    size total_len = nmemb * element_size;
    void *p = malloc(total_len);
    if (p) memset(p, 0, total_len);
    return p;
}
