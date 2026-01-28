#include <lib/io.h>
#include <drivers/serial.h>
#include <arch/amd64/context.h>
#include <arch/amd64/cpu.h>
#include <arch/amd64/interrupts.h>
#include <drivers/vt/vt.h>
#include <stdarg.h>

static void panic_print(const char *s) {
    //always dump to serial
    serial_write(s);
    
    //dump to VT if active
    vt_t *vt = vt_get_active();
    if (vt) {
        vt_print(vt, s);
        vt_flush(vt);
    }
}

static void panic_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    panic_print(buf);
    va_end(args);
}

void kpanic(void *p_frame, const char *fmt, ...) {
    interrupt_frame_t *frame = (interrupt_frame_t *)p_frame;
    //disable interrupts immediately
    arch_interrupts_disable();
    
    //RSOD (red screen of death)
    vt_t *vt = vt_get_active();
    if (vt) {
        vt_set_attr(vt, VT_ATTR_BG, 0xAA0000); //dark red
        vt_set_attr(vt, VT_ATTR_FG, 0xFFFFFF); //white
        vt_clear(vt);
        vt_flush(vt);
    }
    
    panic_printf("\n*** KERNEL PANIC ***\n\n");
    
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, args);
        panic_print(buf);
        panic_print("\n");
        va_end(args);
    }
    
    if (frame) {
        panic_printf("\nFault Details:\n");
        panic_printf("  Vector:     0x%lX\n", frame->vector);
        panic_printf("  Error code: 0x%lX\n", frame->error_code);
        panic_printf("  RIP:        0x%lX (CS: 0x%lX)\n", frame->rip, frame->cs);
        panic_printf("  RSP:        0x%lX (SS: 0x%lX)\n", frame->rsp, frame->ss);
        panic_printf("  RFLAGS:     0x%lX\n", frame->rflags);
        
        //register dump
        panic_printf("\nRegisters:\n");
        panic_printf("  RAX: 0x%016lx RBX: 0x%016lx RCX: 0x%016lx\n", frame->rax, frame->rbx, frame->rcx);
        panic_printf("  RDX: 0x%016lx RSI: 0x%016lx RDI: 0x%016lx\n", frame->rdx, frame->rsi, frame->rdi);
        panic_printf("  RBP: 0x%016lx R8:  0x%016lx R9:  0x%016lx\n", frame->rbp, frame->r8, frame->r9);
        panic_printf("  R10: 0x%016lx R11: 0x%016lx R12: 0x%016lx\n", frame->r10, frame->r11, frame->r12);
        panic_printf("  R13: 0x%016lx R14: 0x%016lx R15: 0x%016lx\n", frame->r13, frame->r14, frame->r15);
    }
    
    panic_printf("\n--- System Halted ---\n");
    
    //halt forever
    for (;;) arch_halt();
}
