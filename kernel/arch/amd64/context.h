#ifndef ARCH_AMD64_CONTEXT_H
#define ARCH_AMD64_CONTEXT_H

#include <arch/types.h>


//AMD64 context - System V ABI layout
//callee-saved: rbx, rbp, r12-r15
//caller-saved: rax, rcx, rdx, rsi, rdi, r8-r11
struct context {
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
};

typedef struct context context_t;

//context functions
void context_init(context_t *ctx, void *stack_top, void (*entry)(void *), void *arg);
void context_init_user(context_t *ctx, void *user_stack, void *entry, void *arg);
uint64 context_get_pc(context_t *ctx);
void context_set_pc(context_t *ctx, uint64 pc);
uint64 context_get_sp(context_t *ctx);
void context_set_sp(context_t *ctx, uint64 sp);
uint64 context_get_retval(context_t *ctx);
void context_set_retval(context_t *ctx, uint64 val);
void context_get_syscall_args(context_t *ctx, uint64 *args, int count);
void context_switch(context_t *old_ctx, context_t *new_ctx);
void enter_usermode(context_t *ctx);
void return_to_usermode(context_t *ctx);

#endif

