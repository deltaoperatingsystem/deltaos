#include <arch/types.h>
#include <arch/io.h>
#include <arch/amd64/percpu.h>
#include <lib/io.h>

#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_FMASK  0xC0000084

#define EFER_SCE    (1ULL << 0)

#define KERNEL_CS   0x08
#define USER_SS     0x18

extern void syscall_entry_simple(void);

void syscall_init(void) {
    percpu_init();
    
    uint64 star = ((uint64)(USER_SS) << 48) | ((uint64)KERNEL_CS << 32);
    wrmsr(IA32_STAR, star);
    
    wrmsr(IA32_LSTAR, (uint64)&syscall_entry_simple);
    
    uint64 fmask = (1ULL << 9) | (1ULL << 8) | (1ULL << 10);
    wrmsr(IA32_FMASK, fmask);
    
    uint64 efer = rdmsr(IA32_EFER);
    efer |= EFER_SCE;
    wrmsr(IA32_EFER, efer);
    
    puts("[syscall] initialized\n");
}
