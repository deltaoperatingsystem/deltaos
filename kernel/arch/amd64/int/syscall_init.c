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
#define SYSRET_BASE 0x10    //SYSRET uses: SS = base+8 (0x18) CS = base+16 (0x20)

extern void syscall_entry_simple(void);

void syscall_init(void) {
    percpu_init();
    
    //STAR MSR format:
    //[63:48] = SYSRET CS/SS base (SS = base+8 CS = base+16 for 64-bit)
    //[47:32] = SYSCALL CS/SS base (CS = base SS = base+8)
    uint64 star = ((uint64)SYSRET_BASE << 48) | ((uint64)KERNEL_CS << 32);
    wrmsr(IA32_STAR, star);
    
    wrmsr(IA32_LSTAR, (uint64)&syscall_entry_simple);
    
    uint64 fmask = (1ULL << 9) | (1ULL << 8) | (1ULL << 10);
    wrmsr(IA32_FMASK, fmask);
    
    uint64 efer = rdmsr(IA32_EFER);
    efer |= EFER_SCE;
    wrmsr(IA32_EFER, efer);
    
    puts("[syscall] initialized\n");
}
