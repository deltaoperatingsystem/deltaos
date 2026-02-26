;struct arch_context offsets (must match context.h exactly)
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

%define KERNEL_DS   0x10
%define THREAD_CTX_OFFSET 48

%macro isr_err_stub 1
isr_stub_%+%1:
    push qword %1
    jmp common_stub
%endmacro

%macro isr_no_err_stub 1
isr_stub_%+%1:
    push qword 0
    push qword %1
    jmp common_stub
%endmacro

extern interrupt_handler
extern thread_current
extern arch_return_to_usermode

common_stub:
    ;save ALL registers to stack
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ;push placeholder for swapgs flag [rsp+0]
    push qword 0

    ;stack layout at this point (RSP = base, offsets from base):
    ;[base+0]=flag [base+8]=r15 ... [base+120]=rax
    ;[base+128]=vector [base+136]=error
    ;[base+144]=RIP [base+152]=CS [base+160]=RFLAGS
    ;if priv change (user->kernel): [base+168]=RSP [base+176]=SS

    ;if from usermode (CS RPL != 0): always swapgs
    ;if from kernel (CS RPL == 0): check IA32_GS_BASE MSR
    ;uf high 32 bits are 0 GS still points to user TLS (we hit the syscall-entry window)
    ;swapgs in that case too
    mov rax, [rsp + 152]    ;CS
    test rax, 3             ;RPL bits nonzero = usermode
    jnz .do_swapgs

    ;paranoid GS check via MSR
    push rdx
    push rcx
    mov rcx, 0xC0000101     ;IA32_GS_BASE MSR
    rdmsr                   ;edx:eax = current GS_BASE
    test edx, edx           ;kernel addresses have high bits set (signed negative)
    pop rcx
    pop rdx
    js .skip_swapgs         ;high bits set = already kernel GS, no swap needed

.do_swapgs:
    swapgs
    mov qword [rsp + 0], 1  ;set flag to indicate we swapped

.skip_swapgs:
    ;context save (usermode only)
    ;only save to thread context when interrupted from user mode
    ;kernel->kernel interrupts don't context-switch here, restore from stack via iretq
    test qword [rsp + 152], 3
    jz .skip_save

    call thread_current
    test rax, rax
    jz .skip_save

    mov r8, rax
    add r8, THREAD_CTX_OFFSET   ;r8 = &current_thread->context

    ;registers
    mov rax, [rsp + 120] ;rax
    mov [r8 + CTX_RAX], rax
    mov rax, [rsp + 112] ;rbx
    mov [r8 + CTX_RBX], rax
    mov rax, [rsp + 104] ;rcx
    mov [r8 + CTX_RCX], rax
    mov rax, [rsp + 96]  ;rdx
    mov [r8 + CTX_RDX], rax
    mov rax, [rsp + 88]  ;rsi
    mov [r8 + CTX_RSI], rax
    mov rax, [rsp + 80]  ;rdi
    mov [r8 + CTX_RDI], rax
    mov rax, [rsp + 72]  ;rbp
    mov [r8 + CTX_RBP], rax
    mov rax, [rsp + 64]  ;r8
    mov [r8 + CTX_R8], rax
    mov rax, [rsp + 56]  ;r9
    mov [r8 + CTX_R9], rax
    mov rax, [rsp + 48]  ;r10
    mov [r8 + CTX_R10], rax
    mov rax, [rsp + 40]  ;r11
    mov [r8 + CTX_R11], rax
    mov rax, [rsp + 32]  ;r12
    mov [r8 + CTX_R12], rax
    mov rax, [rsp + 24]  ;r13
    mov [r8 + CTX_R13], rax
    mov rax, [rsp + 16]  ;r14
    mov [r8 + CTX_R14], rax
    mov rax, [rsp + 8]   ;r15
    mov [r8 + CTX_R15], rax

    ;interrupt frame (common fields)
    mov rax, [rsp + 144] ;RIP
    mov [r8 + CTX_RIP], rax
    mov rax, [rsp + 152] ;CS
    mov [r8 + CTX_CS], rax
    mov rax, [rsp + 160] ;RFLAGS
    mov [r8 + CTX_RFLAGS], rax

    ;user->kernel: CPU pushed RSP and SS (privilege change occurred)
    mov rax, [rsp + 168] ;user RSP
    mov [r8 + CTX_RSP], rax
    mov rax, [rsp + 176] ;user SS
    mov [r8 + CTX_SS], rax

.skip_save:
    ;call C interrupt handler(vector, error_code, rip, frame*)
    mov rdi, [rsp + 128] ;vector
    mov rsi, [rsp + 136] ;error code
    mov rdx, [rsp + 144] ;RIP
    mov rcx, rsp         ;interrupt_frame_t* pointer
    sub rsp, 8           ;align stack to 16 bytes (ABI)
    call interrupt_handler
    add rsp, 8

    ;check where we came from to choose return path
    test qword [rsp + 152], 3
    jnz .user_return

.manual_restore:
    ;kernel-mode return: restore all regs from stack and iretq
    ;undo any swapgs performed on entry using our stack flag
    cmp qword [rsp + 0], 1
    jne .no_swapgs_out
    swapgs
.no_swapgs_out:
    add rsp, 8      ;discard swapgs flag
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16     ;discard vector and error code
    iretq

.user_return:
    ;usermode return: use thread context (may have switched to a different user thread)
    call thread_current
    test rax, rax
    jz .manual_restore   ;no thread (shouldn't happen)

    mov rdi, rax
    add rdi, THREAD_CTX_OFFSET
    jmp arch_return_to_usermode  ;handles swapgs, sets up iretq frame

;CPU exceptions (0-31)
isr_no_err_stub 0
isr_no_err_stub 1
isr_no_err_stub 2
isr_no_err_stub 3
isr_no_err_stub 4
isr_no_err_stub 5
isr_no_err_stub 6
isr_no_err_stub 7
isr_err_stub    8
isr_no_err_stub 9
isr_err_stub    10
isr_err_stub    11
isr_err_stub    12
isr_err_stub    13
isr_err_stub    14
isr_no_err_stub 15
isr_no_err_stub 16
isr_err_stub    17
isr_no_err_stub 18
isr_no_err_stub 19
isr_no_err_stub 20
isr_no_err_stub 21
isr_no_err_stub 22
isr_no_err_stub 23
isr_no_err_stub 24
isr_no_err_stub 25
isr_no_err_stub 26
isr_no_err_stub 27
isr_no_err_stub 28
isr_no_err_stub 29
isr_err_stub    30
isr_no_err_stub 31

;IRQs (32-255)
%assign i 32
%rep 224
    isr_no_err_stub i
%assign i i+1
%endrep

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep