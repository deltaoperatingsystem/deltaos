#include <arch/types.h>
#include <arch/amd64/cpu.h>

typedef uint64 u64_ua __attribute__((aligned(1), may_alias));
typedef uint32 u32_ua __attribute__((aligned(1), may_alias));

static int has_erms = 0;
static int has_fsrm = 0;

void arch_string_init(void) {
    uint32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    arch_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 7) {
        arch_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        has_erms = (ebx & (1u << 9)) != 0;
        has_fsrm = (edx & (1u << 4)) != 0;
    }
}

static inline void copy_small_32(uint8 *d, const uint8 *s, size n) {
    if (n >= 16) {
        uint64 h0 = *(const u64_ua *)(s);
        uint64 h1 = *(const u64_ua *)(s + 8);
        uint64 t0 = *(const u64_ua *)(s + n - 16);
        uint64 t1 = *(const u64_ua *)(s + n - 8);
        *(u64_ua *)(d)          = h0;
        *(u64_ua *)(d + 8)      = h1;
        *(u64_ua *)(d + n - 16) = t0;
        *(u64_ua *)(d + n - 8)  = t1;
        return;
    }
    if (n >= 8) {
        uint64 h = *(const u64_ua *)(s);
        uint64 t = *(const u64_ua *)(s + n - 8);
        *(u64_ua *)(d)         = h;
        *(u64_ua *)(d + n - 8) = t;
        return;
    }
    if (n >= 4) {
        uint32 h = *(const u32_ua *)(s);
        uint32 t = *(const u32_ua *)(s + n - 4);
        *(u32_ua *)(d)         = h;
        *(u32_ua *)(d + n - 4) = t;
        return;
    }
    if (n > 0) {
        uint8 b0 = s[0];
        uint8 b1 = s[n >> 1];
        uint8 b2 = s[n - 1];
        d[0]      = b0;
        d[n >> 1] = b1;
        d[n - 1]  = b2;
    }
}

void *memcpy(void *dest, const void *src, size n) {
    uint8 *d = (uint8 *)dest;
    const uint8 *s = (const uint8 *)src;

    //fast path for tiny and small copies: overlapping GPR loads/stores
    //avoids rep movsb/movsq microcode startup overhead completely
    if (n <= 32) {
        copy_small_32(d, s, n);
        return dest;
    }

    if (n <= 64) {
        uint64 h0 = *(const u64_ua *)(s);
        uint64 h1 = *(const u64_ua *)(s + 8);
        uint64 h2 = *(const u64_ua *)(s + 16);
        uint64 h3 = *(const u64_ua *)(s + 24);
        *(u64_ua *)(d)      = h0;
        *(u64_ua *)(d + 8)  = h1;
        *(u64_ua *)(d + 16) = h2;
        *(u64_ua *)(d + 24) = h3;
        uint64 t0 = *(const u64_ua *)(s + n - 32);
        uint64 t1 = *(const u64_ua *)(s + n - 24);
        uint64 t2 = *(const u64_ua *)(s + n - 16);
        uint64 t3 = *(const u64_ua *)(s + n - 8);
        *(u64_ua *)(d + n - 32) = t0;
        *(u64_ua *)(d + n - 24) = t1;
        *(u64_ua *)(d + n - 16) = t2;
        *(u64_ua *)(d + n - 8)  = t3;
        return dest;
    }

    //medium copies (65..255 bytes)
    if (n < 256) {
        if (has_fsrm && n <= 128) {
            void *di = dest;
            const void *si = src;
            size cnt = n;
            __asm__ volatile (
                "rep movsb"
                : "+D"(di), "+S"(si), "+c"(cnt)
                :
                : "memory"
            );
            return dest;
        }

        //unrolled 32-byte GPR loop + overlapping 32-byte tail
        const uint8 *s_tail = s + n - 32;
        uint8 *d_tail = d + n - 32;
        do {
            uint64 v0 = *(const u64_ua *)(s);
            uint64 v1 = *(const u64_ua *)(s + 8);
            uint64 v2 = *(const u64_ua *)(s + 16);
            uint64 v3 = *(const u64_ua *)(s + 24);
            *(u64_ua *)(d)      = v0;
            *(u64_ua *)(d + 8)  = v1;
            *(u64_ua *)(d + 16) = v2;
            *(u64_ua *)(d + 24) = v3;
            s += 32;
            d += 32;
        } while (d < d_tail);
        uint64 t0 = *(const u64_ua *)(s_tail);
        uint64 t1 = *(const u64_ua *)(s_tail + 8);
        uint64 t2 = *(const u64_ua *)(s_tail + 16);
        uint64 t3 = *(const u64_ua *)(s_tail + 24);
        *(u64_ua *)(d_tail)      = t0;
        *(u64_ua *)(d_tail + 8)  = t1;
        *(u64_ua *)(d_tail + 16) = t2;
        *(u64_ua *)(d_tail + 24) = t3;
        return dest;
    }

    //large copies (n >= 256)
    if (has_erms || has_fsrm) {
        void *di = dest;
        const void *si = src;
        size cnt = n;
        __asm__ volatile (
            "rep movsb"
            : "+D"(di), "+S"(si), "+c"(cnt)
            :
            : "memory"
        );
        return dest;
    }

    //fallback without ERMS: rep movsq for 8-byte chunks, single overlapping qword for tail
    void *di = dest;
    const void *si = src;
    size qwords = n >> 3;
    __asm__ volatile (
        "rep movsq"
        : "+D"(di), "+S"(si), "+c"(qwords)
        :
        : "memory"
    );
    if (n & 7) {
        *(u64_ua *)(d + n - 8) = *(const u64_ua *)(s + n - 8);
    }

    return dest;
}

void *memset(void *s, int c, size n) {
    uint8 *d = (uint8 *)s;
    uint8 val = (uint8)c;
    uint64 pat = (uint64)val * 0x0101010101010101ULL;

    //fast path for tiny and small fills (0..64 bytes) so there's  (practically) zero microcode overhead
    if (n <= 64) {
        if (n >= 32) {
            *(u64_ua *)(d) = pat;
            *(u64_ua *)(d + 8) = pat;
            *(u64_ua *)(d + 16) = pat;
            *(u64_ua *)(d + 24) = pat;
            *(u64_ua *)(d + n - 32) = pat;
            *(u64_ua *)(d + n - 24) = pat;
            *(u64_ua *)(d + n - 16) = pat;
            *(u64_ua *)(d + n - 8)  = pat;
            return s;
        }
        if (n >= 16) {
            *(u64_ua *)(d) = pat;
            *(u64_ua *)(d + 8) = pat;
            *(u64_ua *)(d + n - 16) = pat;
            *(u64_ua *)(d + n - 8)  = pat;
            return s;
        }
        if (n >= 8) {
            *(u64_ua *)(d) = pat;
            *(u64_ua *)(d + n - 8) = pat;
            return s;
        }
        if (n >= 4) {
            uint32 pat32 = (uint32)pat;
            *(u32_ua *)(d) = pat32;
            *(u32_ua *)(d + n - 4) = pat32;
            return s;
        }
        if (n > 0) {
            d[0]      = val;
            d[n >> 1] = val;
            d[n - 1]  = val;
        }
        return s;
    }

    //medium fills (65..255 bytes): unrolled 32-byte GPR loop + overlapping tail
    if (n < 256) {
        uint8 *end = d + n - 32;
        do {
            *(u64_ua *)(d)      = pat;
            *(u64_ua *)(d + 8)  = pat;
            *(u64_ua *)(d + 16) = pat;
            *(u64_ua *)(d + 24) = pat;
            d += 32;
        } while (d < end);
        *(u64_ua *)(end)      = pat;
        *(u64_ua *)(end + 8)  = pat;
        *(u64_ua *)(end + 16) = pat;
        *(u64_ua *)(end + 24) = pat;
        return s;
    }

    //large fills (n >= 256)
    if (has_erms) {
        void *di = s;
        size cnt = n;
        __asm__ volatile (
            "rep stosb"
            : "+D"(di), "+c"(cnt)
            : "a"(val)
            : "memory"
        );
        return s;
    }

    //fallback without ERMS: rep stosq + single overlapping 8-byte tail store
    void *di = s;
    size qwords = n >> 3;
    __asm__ volatile (
        "rep stosq"
        : "+D"(di), "+c"(qwords)
        : "a"(pat)
        : "memory"
    );
    if (n & 7) {
        *(u64_ua *)(d + n - 8) = pat;
    }

    return s;
}

void *memmove(void *dest, const void *src, size n) {
    uint8 *d = (uint8 *)dest;
    const uint8 *s = (const uint8 *)src;

    //small copies (0..32) load all registers before storing, so they are overlap-safe in any direction
    if (n <= 32) {
        copy_small_32(d, s, n);
        return dest;
    }

    //non-overlapping in both directions: use  memcpy
    if ((uintptr)d - (uintptr)s >= n && (uintptr)s - (uintptr)d >= n) {
        return memcpy(dest, src, n);
    }

    if (d < s) {
        //overlapping forward copy (d < s < d + n) for n > 32:
        //save the last 16 bytes before forward loop overwrites them
        uint64 t0 = *(const u64_ua *)(s + n - 16);
        uint64 t1 = *(const u64_ua *)(s + n - 8);

        uint8 *d_cur = d;
        const uint8 *s_cur = s;
        uint8 *d_stop = d + n - 16;

        do {
            uint64 v0 = *(const u64_ua *)(s_cur);
            uint64 v1 = *(const u64_ua *)(s_cur + 8);
            *(u64_ua *)(d_cur)     = v0;
            *(u64_ua *)(d_cur + 8) = v1;
            d_cur += 16;
            s_cur += 16;
        } while (d_cur < d_stop);

        *(u64_ua *)(d_stop)     = t0;
        *(u64_ua *)(d_stop + 8) = t1;
        return dest;
    }

    if (d > s) {
        //overlapping backward copy (s < d < s + n) for n > 32:
        //save the first 16 bytes before backward loop overwrites them
        uint64 h0 = *(const u64_ua *)(s);
        uint64 h1 = *(const u64_ua *)(s + 8);

        uint8 *d_cur = d + n;
        const uint8 *s_cur = s + n;
        uint8 *d_stop = d + 16;

        do {
            d_cur -= 16;
            s_cur -= 16;
            uint64 v0 = *(const u64_ua *)(s_cur);
            uint64 v1 = *(const u64_ua *)(s_cur + 8);
            *(u64_ua *)(d_cur)     = v0;
            *(u64_ua *)(d_cur + 8) = v1;
        } while (d_cur > d_stop);

        *(u64_ua *)(d)     = h0;
        *(u64_ua *)(d + 8) = h1;
    }

    return dest;
}
