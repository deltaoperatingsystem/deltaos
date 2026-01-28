#include <string.h>
#include <types.h>

//ptimized asm implementations

void *memcpy(void *dest, const void *src, size n) {
    void *d = dest;
    const void *s = src;
    size cnt = n;
    
    __asm__ volatile (
        "mov %2, %%rcx\n"
        "shr $3, %%rcx\n"
        "rep movsq\n"
        "mov %2, %%rcx\n"
        "and $7, %%rcx\n"
        "rep movsb\n"
        : "+D"(d), "+S"(s), "+r"(cnt)
        :
        : "rcx", "memory"
    );
    return dest;
}

void *memset(void *s, int c, size n) {
    void *d = s;
    size cnt = n;
    unsigned char val = (unsigned char)c;
    unsigned long long word_val = (val * 0x0101010101010101ULL);
    
    __asm__ volatile (
        "mov %1, %%rcx\n"
        "shr $3, %%rcx\n"
        "rep stosq\n"
        "mov %1, %%rcx\n"
        "and $7, %%rcx\n"
        "rep stosb\n"
        : "+D"(d), "+r"(cnt)
        : "a"(word_val)
        : "rcx", "memory"
    );
    return s;
}

void *memmove(void *dest, const void *src, size n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        return memcpy(dest, src, n);
    } else if (d > s) {
        d += n;
        s += n;
        //simple backward loop for correctness
        for (size i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}
