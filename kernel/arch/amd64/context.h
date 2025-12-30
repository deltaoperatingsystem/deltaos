#ifndef ARCH_AMD64_CONTEXT_H
#define ARCH_AMD64_CONTEXT_H

#include <arch/types.h>


//AMD64 context - System V ABI layout
//callee-saved: rbx, rbp, r12-r15
//caller-saved: rax, rcx, rdx, rsi, rdi, r8-r11
struct arch_context {
    //callee-saved registers (preserved across calls)
    uint64 rbx;
    uint64 rbp;
    uint64 r12;
    uint64 r13;
    uint64 r14;
    uint64 r15;
    
    //instruction pointer and stack
    uint64 rip;
    uint64 rsp;
    
    //flags and segments (for ring transitions)
    uint64 rflags;
    uint64 cs;
    uint64 ss;
    
    //syscall args / scratch (not preserved but needed for syscall handling)
    uint64 rax;  //syscall number / return value
    uint64 rdi;  //arg1
    uint64 rsi;  //arg2
    uint64 rdx;  //arg3
    uint64 r10;  //arg4 (rcx clobbered by syscall)
    uint64 r8;   //arg5
    uint64 r9;   //arg6
    uint64 r11;  //rflags saved by syscall instruction
    uint64 rcx;  //clobbered by syscall, saved for interrupt returns
};

typedef struct arch_context arch_context_t;

//context functions
void arch_context_init(arch_context_t *ctx, void *stack_top, void (*entry)(void *), void *arg);
void arch_context_init_user(arch_context_t *ctx, void *user_stack, void *entry, void *arg);
uint64 arch_context_get_pc(arch_context_t *ctx);
void arch_context_set_pc(arch_context_t *ctx, uint64 pc);
uint64 arch_context_get_sp(arch_context_t *ctx);
void arch_context_set_sp(arch_context_t *ctx, uint64 sp);
uint64 arch_context_get_retval(arch_context_t *ctx);
void arch_context_set_retval(arch_context_t *ctx, uint64 val);
void arch_context_get_syscall_args(arch_context_t *ctx, uint64 *args, int count);
void arch_context_switch(arch_context_t *old_ctx, arch_context_t *new_ctx);
void arch_context_load(arch_context_t *ctx); 
void arch_enter_usermode(arch_context_t *ctx);
void arch_return_to_usermode(arch_context_t *ctx);

#endif

