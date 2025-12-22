#include <arch/context.h>
#include <arch/amd64/context.h>
#include <lib/string.h>

void arch_context_init(arch_context_t *ctx, void *stack_top, void (*entry)(void *), void *arg) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->rsp = (uint64)stack_top;
    ctx->rip = (uint64)entry;
    ctx->rdi = (uint64)arg;       //first arg in System V ABI
    ctx->rflags = 0x202;          //IF=1 (interrupts enabled)
    ctx->cs = 0x08;               //kernel code segment
    ctx->ss = 0x10;               //kernel data segment
}

uint64 arch_context_get_pc(arch_context_t *ctx) {
    return ctx->rip;
}

void arch_context_set_pc(arch_context_t *ctx, uint64 pc) {
    ctx->rip = pc;
}

uint64 arch_context_get_sp(arch_context_t *ctx) {
    return ctx->rsp;
}

void arch_context_set_sp(arch_context_t *ctx, uint64 sp) {
    ctx->rsp = sp;
}

uint64 arch_context_get_retval(arch_context_t *ctx) {
    return ctx->rax;
}

void arch_context_set_retval(arch_context_t *ctx, uint64 val) {
    ctx->rax = val;
}

void arch_context_get_syscall_args(arch_context_t *ctx, uint64 *args, int count) {
    //System V AMD64 syscall ABI: rdi, rsi, rdx, r10, r8, r9
    if (count > 0) args[0] = ctx->rdi;
    if (count > 1) args[1] = ctx->rsi;
    if (count > 2) args[2] = ctx->rdx;
    if (count > 3) args[3] = ctx->r10;
    if (count > 4) args[4] = ctx->r8;
    if (count > 5) args[5] = ctx->r9;
}

void arch_context_init_user(arch_context_t *ctx, void *user_stack, void *entry, void *arg) {
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->rsp = (uint64)user_stack;
    ctx->rip = (uint64)entry;
    ctx->rdi = (uint64)arg;       //first arg in System V ABI
    ctx->rflags = 0x202;          //IF=1 (interrupts enabled)
    ctx->cs = 0x20 | 3;           //user code segment RPL=3
    ctx->ss = 0x18 | 3;           //user data segment RPL=3
}
