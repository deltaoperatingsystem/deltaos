#include <arch/types.h>
#include <arch/amd64/cpu.h>

//TODO ??? this is weird i suspect this might have regressed perforamcce as fuck on real hardware but idk why

static int cpu_features_detected = 0;
static int has_erms = 0;
static int has_fsrm = 0;

void arch_string_init(void) {
    uint32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    arch_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 7) {
        arch_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        has_erms = (ebx & (1 << 9)) != 0;
        has_fsrm = (edx & (1 << 4)) != 0;
    }
    cpu_features_detected = 1;
}

void *memcpy(void *dest, const void *src, size n) {
    if (n == 0) return dest;

    //use fsrm if available
    if (has_fsrm) {
        void *d = dest;
        const void *s = src;
        size cnt = n;
        __asm__ volatile (
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(cnt) //outputs (modified)
            : //inputs (in registers via constraints)
            : "memory" //clobbers
        );
        return dest;
    }

    //use erms for larger copies
    if (has_erms && n >= 256) {
        void *d = dest;
        const void *s = src;
        size cnt = n;
        __asm__ volatile (
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(cnt) //outputs (modified)
            : //inputs (in registers via constraints)
            : "memory" //clobbers
        );
        return dest;
    }

    //fallback: standard optimized rep movsq + rep movsb
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
    if (n == 0) return s;

    unsigned char val = (unsigned char)c;

    //use erms for larger clears
    if (has_erms && n >= 256) {
        void *d = s;
        size cnt = n;
        __asm__ volatile (
            "rep stosb"
            : "+D"(d), "+c"(cnt) //outputs (modified)
            : "a"(val) //inputs (in registers via constraints)
            : "memory" //clobbers
        );
        return s;
    }

    void *d = s;
    size cnt = n;
    unsigned long long word_val = (val * 0x0101010101010101ULL);

    __asm__ volatile (
        "mov %1, %%rcx\n\t"
        "shr $3, %%rcx\n\t"     //rcx = n / 8
        "rep stosq\n\t"         //fill 8-byte chunks
        "mov %1, %%rcx\n\t"
        "and $7, %%rcx\n\t"     //rcx = n % 8
        "rep stosb"             //fill remaining bytes
        : "+D"(d), "+r"(cnt) //outputs (modified)
        : "a"(word_val) //inputs (in registers via constraints)
        : "rcx", "memory" //clobbers
    );

    return s;
}

void *memmove(void *dest, const void *src, size n) {
    if (dest == src || n == 0) return dest;

    if (dest < src) {
        //forward copy - just use memcpy
        return memcpy(dest, src, n);
    } else {
        //backward copy using loop to avoid direction flag (df) bugs
        unsigned char *d = (unsigned char *)dest + n;
        const unsigned char *s = (const unsigned char *)src + n;
        size cnt = n;

        if (cnt >= 8) {
            __asm__ volatile (
                "1:\n\t"
                "sub $8, %0\n\t"
                "sub $8, %1\n\t"
                "movq (%1), %%rax\n\t"
                "movq %%rax, (%0)\n\t"
                "sub $8, %2\n\t"
                "cmp $8, %2\n\t"
                "jae 1b"
                : "+r"(d), "+r"(s), "+r"(cnt) //outputs (modified)
                : //inputs (in registers via constraints)
                : "rax", "cc", "memory" //clobbers
            );
        }

        if (cnt > 0) {
            __asm__ volatile (
                "2:\n\t"
                "dec %0\n\t"
                "dec %1\n\t"
                "movb (%1), %%al\n\t"
                "movb %%al, (%0)\n\t"
                "dec %2\n\t"
                "jnz 2b"
                : "+r"(d), "+r"(s), "+r"(cnt) //outputs (modified)
                : //inputs (in registers via constraints)
                : "al", "cc", "memory" //clobbers
            );
        }
    }

    return dest;
}
