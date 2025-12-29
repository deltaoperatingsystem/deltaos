#include <arch/amd64/percpu.h>
#include <arch/io.h>
#include <lib/io.h>

#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

static percpu_t boot_percpu;

percpu_t *percpu_get(void) {
    percpu_t *cpu;
    __asm__ volatile ("mov %%gs:%c1, %0" : "=r"(cpu) : "i"(PERCPU_SELF));
    return cpu;
}

void percpu_init(void) {
    boot_percpu.kernel_rsp = 0;
    boot_percpu.user_rsp = 0;
    boot_percpu.current_thread = NULL;
    boot_percpu.self = &boot_percpu;
    
    wrmsr(IA32_KERNEL_GS_BASE, (uint64)&boot_percpu);
    wrmsr(IA32_GS_BASE, (uint64)&boot_percpu);
    
    puts("[percpu] initialized\n");
}

void percpu_set_kernel_stack(void *stack_top) {
    percpu_t *cpu = percpu_get();
    cpu->kernel_rsp = (uint64)stack_top;
}
