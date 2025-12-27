#include <arch/types.h>
#include <arch/io.h>

#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

void syscall_init(void) {
    uint16 kernel_cs = 0x08;
    uint16 user_cs = 0x20;

    uint64 star = ((uint64)user_cs << 48) | ((uint64)kernel_cs << 32);
    wrmsr(IA32_LSTAR, star);

    extern void syscall_entry_stub(void);
    wrmsr(IA32_LSTAR, (uint64)&syscall_entry_stub);
    
    uint64 fmask = (1ULL << 9);
    wrmsr(IA32_FMASK, fmask);

    uint64 efer = rdmsr(IA32_EFER);
    efer |= 1;
    wrmsr(IA32_EFER, efer);
}

void syscall_handler(void) {
    extern void puts(char *s);
    puts("HELLO WORLD!!");
}