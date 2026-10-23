#include <arch/types.h>
#include <lib/mem.h>

word ctoh(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

word atoi(const char *p) {
    //skip leading whitespace
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;

    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    } else if (*p == '+') {
        p++;
    }

    //accumulate unsigned and saturate instead of overflowing
    uword limit = neg ? ((uword)(~(uword)0 >> 1) + 1) : (uword)(~(uword)0 >> 1);
    uword k = 0;
    while (*p >= '0' && *p <= '9') {
        uword d = (uword)(*p - '0');
        if (k > (limit - d) / 10) {
            k = limit;
            break;
        }
        k = k * 10 + d;
        p++;
    }
    return neg ? (word)(0 - k) : (word)k;
}

typedef uint64 u64_ua __attribute__((aligned(1), may_alias));
typedef uint32 u32_ua __attribute__((aligned(1), may_alias));
typedef uword uword_ua __attribute__((aligned(1), may_alias));

//stop gcc from turning the fallback copy/fill loops below into calls to themselves
#define NO_LOOP_IDIOMS __attribute__((optimize("no-tree-loop-distribute-patterns")))

#define SWAR_ONES 0x0101010101010101ULL
#define SWAR_HIGHS 0x8080808080808080ULL

size strlen(const char *s) {
    const char *p = s;

    //align pointer to 8-byte boundary so word loads never cross a page boundary
    while (((uintptr)p & (sizeof(uint64) - 1)) != 0) {
        if (*p == '\0') return (size)(p - s);
        p++;
    }

    const uint64 *wp = (const uint64 *)p;
    for (;;) {
        uint64 w = *wp;
        uint64 zero = (w - SWAR_ONES) & ~w & SWAR_HIGHS;
        if (zero != 0) {
            return (size)((const char *)wp - s) + (size)(__builtin_ctzll(zero) >> 3);
        }
        wp++;
    }
}

int strcmp(const char *s1, const char *s2) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    //align p1 to 8-byte boundary so loads from p1 never cross a page boundary
    while (((uintptr)p1 & (sizeof(uint64) - 1)) != 0) {
        unsigned char c1 = *p1++;
        unsigned char c2 = *p2++;
        if (c1 != c2 || c1 == '\0') {
            return (int)c1 - (int)c2;
        }
    }

    for (;;) {
        //fast word-at-a-time loop while unaligned load from p2 stays within its 4 KiB page
        while (((uintptr)p2 & 0xFFFU) <= (0x1000U - sizeof(uint64))) {
            uint64 w1 = *(const uint64 *)p1;
            uint64 w2 = *(const u64_ua *)p2;
            uint64 zero = (w1 - SWAR_ONES) & ~w1 & SWAR_HIGHS;
            uint64 diff = (w1 ^ w2) | zero;
            if (diff != 0) {
                size idx = (size)(__builtin_ctzll(diff) >> 3);
                return (int)p1[idx] - (int)p2[idx];
            }
            p1 += sizeof(uint64);
            p2 += sizeof(uint64);
        }

        //step 8 bytes across p2's page boundary while preserving p1's 8-byte alignment
        for (size i = 0; i < sizeof(uint64); i++) {
            unsigned char c1 = *p1++;
            unsigned char c2 = *p2++;
            if (c1 != c2 || c1 == '\0') {
                return (int)c1 - (int)c2;
            }
        }
    }
}

int strncmp(const char *s1, const char *s2, size n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size n) {
    char *d = dest;
    size i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    for (; i < n; i++) d[i] = 0;
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return NULL;
}

//reentrant tokenizer: all state lives in *saveptr
char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *next = str ? str : *saveptr;
    if (!next) return NULL;

    //skip leading delimiters
    while (*next && strchr(delim, *next)) next++;
    if (!*next) {
        *saveptr = NULL;
        return NULL;
    }

    char *start = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) {
        *next = '\0';
        *saveptr = next + 1;
    } else {
        *saveptr = NULL;
    }
    return start;
}

//weak symbol allows arch-specific override
NO_LOOP_IDIOMS __attribute__((weak)) void *memset(void *s, int c, size n) {
    uint8 *p = (uint8 *)s;
    uint8 val = (uint8)c;
    
    //optimization: fill word by word if size is large enough
    if (n >= sizeof(uword)) {
        uword word_val = val;
        for (size i = 1; i < sizeof(uword); i++) {
            word_val |= (uword)val << (i * 8);
        }
        
        while (n >= sizeof(uword)) {
            *(uword_ua *)p = word_val;
            p += sizeof(uword);
            n -= sizeof(uword);
        }
    }
    
    while (n--) *p++ = val;
    return s;
}

NO_LOOP_IDIOMS __attribute__((weak)) void *memcpy(void *dest, const void *src, size n) {
    uint8 *d = (uint8 *)dest;
    const uint8 *s = (const uint8 *)src;
    
    while (n >= sizeof(uword)) {
        *(uword_ua *)d = *(const uword_ua *)s;
        d += sizeof(uword);
        s += sizeof(uword);
        n -= sizeof(uword);
    }
    
    while (n--) *d++ = *s++;
    return dest;
}

NO_LOOP_IDIOMS __attribute__((weak)) void *memmove(void *dest, const void *src, size n) {
    uint8 *d = (uint8 *)dest;
    const uint8 *s = (const uint8 *)src;

    if (d < s) {
        while (n >= sizeof(uword)) {
            *(uword_ua *)d = *(const uword_ua *)s;
            d += sizeof(uword);
            s += sizeof(uword);
            n -= sizeof(uword);
        }
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n >= sizeof(uword)) {
            d -= sizeof(uword);
            s -= sizeof(uword);
            *(uword_ua *)d = *(const uword_ua *)s;
            n -= sizeof(uword);
        }
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (n >= sizeof(uint64)) {
        uint64 w1 = *(const u64_ua *)p1;
        uint64 w2 = *(const u64_ua *)p2;
        if (w1 != w2) {
            return __builtin_bswap64(w1) < __builtin_bswap64(w2) ? -1 : 1;
        }
        p1 += sizeof(uint64);
        p2 += sizeof(uint64);
        n -= sizeof(uint64);
    }

    if (n >= sizeof(uint32)) {
        uint32 d1 = *(const u32_ua *)p1;
        uint32 d2 = *(const u32_ua *)p2;
        if (d1 != d2) {
            return __builtin_bswap32(d1) < __builtin_bswap32(d2) ? -1 : 1;
        }
        p1 += sizeof(uint32);
        p2 += sizeof(uint32);
        n -= sizeof(uint32);
    }

    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++;
        p2++;
    }
    return 0;
}

char *strdup(const char *s) {
    size len = strlen(s) + 1;
    char *new = malloc(len);
    if (new) memcpy(new, s, len);
    return new;
}
