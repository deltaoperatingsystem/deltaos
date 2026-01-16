section .text

;struct arch_context offsets
%define CTX_RBX     0
%define CTX_RBP     8
%define CTX_R12     16
%define CTX_R13     24
%define CTX_R14     32
%define CTX_R15     40
%define CTX_RIP     48
%define CTX_RSP     56
%define CTX_RFLAGS  64
%define CTX_CS      72
%define CTX_SS      80
%define CTX_RAX     88
%define CTX_RDI     96
%define CTX_RSI     104
%define CTX_RDX     112
%define CTX_R10     120
%define CTX_R8      128
%define CTX_R9      136
%define CTX_R11     144
%define CTX_RCX     152

;kernel segment selectors
%define KERNEL_CS   0x08
%define KERNEL_DS   0x10
%define USER_CS     0x23
%define USER_DS     0x1B

;arch_context_switch(old_ctx, new_ctx)
global arch_context_switch
arch_context_switch:
    mov [rdi + CTX_RAX], rax
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RCX], rcx
    mov [rdi + CTX_RDX], rdx
    mov [rdi + CTX_RSI], rsi
    mov [rdi + CTX_RDI], rdi
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_R8],  r8
    mov [rdi + CTX_R9],  r9
    mov [rdi + CTX_R10], r10
    mov [rdi + CTX_R11], r11
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R15], r15
    mov [rdi + CTX_RSP], rsp
    pushfq
    pop rax
    mov [rdi + CTX_RFLAGS], rax
    mov qword [rdi + CTX_CS], KERNEL_CS
    mov qword [rdi + CTX_SS], KERNEL_DS
    lea rax, [rel .switch_return]
    mov [rdi + CTX_RIP], rax
    
    mov rdi, rsi
    jmp arch_return_to_usermode

.switch_return:
    ret

global arch_context_load
arch_context_load:
    jmp arch_return_to_usermode

global arch_enter_usermode
arch_enter_usermode:
    jmp arch_return_to_usermode

;arch_return_to_usermode(ctx_ptr)
global arch_return_to_usermode
arch_return_to_usermode:
    mov rax, [rdi + CTX_CS]
    and rax, 3
    jz .kernel_return

.user_return:
    swapgs
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    push qword [rdi + CTX_SS]
    push qword [rdi + CTX_RSP]
    push qword [rdi + CTX_RFLAGS]
    push qword [rdi + CTX_CS]
    push qword [rdi + CTX_RIP]
    mov rax, [rdi + CTX_RAX]
    mov rbx, [rdi + CTX_RBX]
    mov rdx, [rdi + CTX_RDX]
    mov rsi, [rdi + CTX_RSI]
    mov rbp, [rdi + CTX_RBP]
    mov r8,  [rdi + CTX_R8]
    mov r9,  [rdi + CTX_R9]
    mov r10, [rdi + CTX_R10]
    mov r11, [rdi + CTX_R11]
    mov r12, [rdi + CTX_R12]
    mov r13, [rdi + CTX_R13]
    mov r14, [rdi + CTX_R14]
    mov r15, [rdi + CTX_R15]
    mov rcx, [rdi + CTX_RCX]
    mov rdi, [rdi + CTX_RDI]
    iretq

.kernel_return:
    mov rsp, [rdi + CTX_RSP]
    mov rax, [rdi + CTX_RFLAGS]
    push rax
    popfq
    mov rax, [rdi + CTX_RAX]
    mov rbx, [rdi + CTX_RBX]
    mov rdx, [rdi + CTX_RDX]
    mov rsi, [rdi + CTX_RSI]
    mov rbp, [rdi + CTX_RBP]
    mov r8,  [rdi + CTX_R8]
    mov r9,  [rdi + CTX_R9]
    mov r10, [rdi + CTX_R10]
    mov r11, [rdi + CTX_R11]
    mov r12, [rdi + CTX_R12]
    mov r13, [rdi + CTX_R13]
    mov r14, [rdi + CTX_R14]
    mov r15, [rdi + CTX_R15]
    push qword [rdi + CTX_RIP]
    mov rcx, [rdi + CTX_RCX]
    mov rdi, [rdi + CTX_RDI]
    ret