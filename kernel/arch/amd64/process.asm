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

;kernel segment selectors
%define KERNEL_CS   0x08
%define KERNEL_DS   0x10
%define USER_CS     0x23    ;0x20 | 3 (RPL=3)
%define USER_DS     0x1B    ;0x18 | 3 (RPL=3)

;arch_context_switch(old_ctx, new_ctx)
;rdi = pointer to old context (to save)
;rsi = pointer to new context (to load)
;saves callee-saved registers to old_ctx and loads from new_ctx
global arch_context_switch
arch_context_switch:
    ;save callee-saved registers to old context
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R15], r15
    
    ;save stack pointer
    mov [rdi + CTX_RSP], rsp
    
    ;save return address
    lea rax, [rel .switch_return]
    mov [rdi + CTX_RIP], rax
    
    ;load new context's callee-saved registers
    mov rbx, [rsi + CTX_RBX]
    mov rbp, [rsi + CTX_RBP]
    mov r12, [rsi + CTX_R12]
    mov r13, [rsi + CTX_R13]
    mov r14, [rsi + CTX_R14]
    mov r15, [rsi + CTX_R15]
    
    ;load new stack pointer
    mov rsp, [rsi + CTX_RSP]
    
    ;jump to new context's saved RIP
    jmp [rsi + CTX_RIP]

.switch_return:
    ;we land here when switched back
    ret

;arch_enter_usermode(ctx)
;rdi = pointer to context with user state
;first-time entry to Ring 3 via iretq
;sets up stack frame for iretq: ss, rsp, rflags, cs, rip
global arch_enter_usermode
arch_enter_usermode:
    ;TODO: swapgs
    ;swapgs
    
    ;load user segment into data segments
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    
    ;build iretq frame on stack
    ;iretq expects: [ss] [rsp] [rflags] [cs] [rip] (bottom to top)
    push qword [rdi + CTX_SS]       ;SS
    push qword [rdi + CTX_RSP]      ;RSP
    push qword [rdi + CTX_RFLAGS]   ;RFLAGS
    push qword [rdi + CTX_CS]       ;CS
    push qword [rdi + CTX_RIP]      ;RIP
    
    ;load argument registers for user entry
    mov rax, [rdi + CTX_RAX]
    mov rsi, [rdi + CTX_RSI]
    mov rdx, [rdi + CTX_RDX]
    mov r10, [rdi + CTX_R10]
    mov r8,  [rdi + CTX_R8]
    mov r9,  [rdi + CTX_R9]
    
    ;load callee-saved (in case user expects them)
    mov rbx, [rdi + CTX_RBX]
    mov rbp, [rdi + CTX_RBP]
    mov r12, [rdi + CTX_R12]
    mov r13, [rdi + CTX_R13]
    mov r14, [rdi + CTX_R14]
    mov r15, [rdi + CTX_R15]
    
    ;load rdi last (it's our context pointer)
    mov rdi, [rdi + CTX_RDI]
    
    ;enter user mode
    iretq

;rdi = pointer to context with saved user state
;returns to usermode after syscall/interrupt
;similar to enter_usermode but handles swapgs correctly
global arch_return_to_usermode
arch_return_to_usermode:
    ;check if returning to user mode (CS has RPL=3)
    mov rax, [rdi + CTX_CS]
    and rax, 3
    jz .kernel_return       ;RPL=0, returning to kernel
    
    ;TODO: Returning to usermode - swap GS
    ;swapgs

.kernel_return:
    ;build iretq frame
    push qword [rdi + CTX_SS]
    push qword [rdi + CTX_RSP]
    push qword [rdi + CTX_RFLAGS]
    push qword [rdi + CTX_CS]
    push qword [rdi + CTX_RIP]
    
    ;restore all general purpose registers
    mov rax, [rdi + CTX_RAX]
    mov rbx, [rdi + CTX_RBX]
    mov rcx, rdi                ;save ctx pointer temporarily in rcx
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
    
    ;restore rdi from the saved context (using rcx as temp)
    mov rdi, [rcx + CTX_RDI]
    
    iretq
