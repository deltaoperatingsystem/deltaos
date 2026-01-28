#include <arch/types.h>

void *memcpy(void *dest, const void *src, size n) {
    //optimized assembly memcpy using rep movsq for maximum throughput
    //good for VRAM write-combining performance
    void *d = dest;
    const void *s = src;
    size cnt = n;
    
    __asm__ volatile (
        "mov %2, %%rcx\n\t"
        "shr $3, %%rcx\n\t"     //rcx = n / 8
        "rep movsq\n\t"         //copy 8-byte chunks
        "mov %2, %%rcx\n\t"
        "and $7, %%rcx\n\t"     //rcx = n % 8
        "rep movsb"             //copy remaining bytes
        : "+D"(d), "+S"(s), "+r"(cnt) //outputs (modified)
        : //inputs (in registers via constraints)
        : "rcx", "memory" //clobbers
    );
    
    return dest;
}

void *memset(void *s, int c, size n) {
    void *d = s;
    size cnt = n;
    unsigned char val = (unsigned char)c;
    unsigned long long word_val = (val * 0x0101010101010101ULL);
    
    __asm__ volatile (
        "mov %1, %%rcx\n\t"
        "shr $3, %%rcx\n\t"     //rcx = n / 8
        "rep stosq\n\t"         //fill 8-byte chunks
        "mov %1, %%rcx\n\t"
        "and $7, %%rcx\n\t"     //rcx = n % 8
        "rep stosb"             //fill remaining bytes
        : "+D"(d), "+r"(cnt)
        : "a"(word_val)
        : "rcx", "memory"
    );
    
    return s;
}

void *memmove(void *dest, const void *src, size n) {
    if (dest == src || n == 0) return dest;

    if (dest < src) {
        //forward copy - just use memcpy
        return memcpy(dest, src, n);
    } else {
        //backward copy
        void *d = (unsigned char *)dest + n;
        const void *s = (const unsigned char *)src + n;
        size cnt = n;
        
        __asm__ volatile (
            "std\n\t"               //set direction flag (backward)
            "lea -8(%%rdi), %%rdi\n\t" //adjust for 8-byte chunks
            "lea -8(%%rsi), %%rsi\n\t"
            "mov %2, %%rcx\n\t"
            "shr $3, %%rcx\n\t"     //rcx = n / 8
            "rep movsq\n\t"         //copy 8-byte chunks
            "and $7, %2\n\t"        //remaining bytes
            "jz 1f\n\t"
            "add $7, %%rdi\n\t"     //adjust back for single bytes
            "add $7, %%rsi\n\t"
            "mov %2, %%rcx\n\t"
            "rep movsb\n\t"
            "1:\n\t"
            "cld"                   //restore direction flag
            : "+D"(d), "+S"(s), "+r"(cnt)
            :
            : "rcx", "memory"
        );
    }
    
    return dest;
}
