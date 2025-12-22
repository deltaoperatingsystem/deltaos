#ifndef ARCH_CONTEXT_H
#define ARCH_CONTEXT_H

#include <arch/types.h>

/*
 * architecture-independent context interface
 * each architecture provides its implementation in arch/<arch>/context.h
 */

#if defined(ARCH_AMD64)
    #include <arch/amd64/context.h>
#elif defined(ARCH_X86)
    #error "x86 not implemented"
#elif defined(ARCH_ARM64)
    #error "ARM64 not implemented"
#else
    #error "Unsupported architecture"
#endif

/*
 * required types:
 * arch_context_t - saved thread/process context (typedef to struct arch_context)
 *
 * required functions:
 * arch_context_init(ctx, stack_top, entry, arg) - setup initial kernel context
 * arch_context_init_user(ctx, stack, entry, arg) - setup initial user context
 * arch_context_get_pc(ctx) - get program counter
 * arch_context_set_pc(ctx, pc) - set program counter
 * arch_context_get_sp(ctx) - get stack pointer
 * arch_context_set_sp(ctx, sp) - set stack pointer
 * arch_context_get_retval(ctx) - get return value register
 * arch_context_set_retval(ctx, val) - set return value register
 * arch_context_get_syscall_args(ctx, args, count) - get syscall arguments
 * arch_context_switch(old, new) - switch between contexts
 * arch_enter_usermode(ctx) - enter usermode for first time
 * arch_return_to_usermode(ctx) - return to usermode after syscall/interrupt
 */

#endif
