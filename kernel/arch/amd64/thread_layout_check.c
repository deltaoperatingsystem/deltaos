#include <proc/thread.h>

_Static_assert(__builtin_offsetof(thread_t, context) == 48, "thread_t.context offset changed");
_Static_assert(__builtin_offsetof(thread_t, user_context) == 224, "thread_t.user_context offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rbx) == 0, "CTX_RBX offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rbp) == 8, "CTX_RBP offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, r12) == 16, "CTX_R12 offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, r13) == 24, "CTX_R13 offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, r14) == 32, "CTX_R14 offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, r15) == 40, "CTX_R15 offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rip) == 48, "CTX_RIP offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rsp) == 56, "CTX_RSP offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rflags) == 64, "CTX_RFLAGS offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rax) == 88, "CTX_RAX offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rdi) == 96, "CTX_RDI offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rsi) == 104, "CTX_RSI offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rdx) == 112, "CTX_RDX offset changed");
_Static_assert(__builtin_offsetof(arch_context_t, rcx) == 152, "CTX_RCX offset changed");
