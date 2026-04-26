bits 64
section .text

%define PERCPU_KERNEL_RSP   0
%define PERCPU_USER_RSP     8
;thread->user_context offset keep in sync with proc/thread.h
;syscall return goes through arch_return_to_usermode now so async events can rewrite the outgoing user frame
%define THREAD_USER_CTX_OFFSET   224

;arch_context field offsets from kernel/arch/amd64/context.h
;this file snapshots the full userspace register image by hand before calling into c
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

%define USER_CS     0x23
%define USER_DS     0x1B

extern syscall_dispatch
extern thread_current
extern proc_prepare_syscall_return
extern arch_return_to_usermode

global syscall_entry_simple
syscall_entry_simple:
    ;syscall enters with user rip in rcx and user rflags in r11
    ;swapgs switches from user gs tls to kernel gs percpu
    swapgs
    mov [gs:PERCPU_USER_RSP], rsp
    mov rsp, [gs:PERCPU_KERNEL_RSP]

    ;save registers clobbered by syscall instruction
    push qword [gs:PERCPU_USER_RSP] ;save REAL user RSP on kernel stack
    push rcx                ;RIP saved by syscall
    push r11                ;RFLAGS saved by syscall
    push rbp
    mov rbp, rsp

    ;save the register file we might clobber before deciding how to return
    ;the rbp relative offsets below depend on this push order
    push rbx
    push r12
    push r13
    push r14
    push r15

    ;save syscall abi arg registers too so we can build a full pre-syscall user snapshot
    push r10
    push r8
    push r9

    ;save the pre-syscall user register image into thread->user_context
    push rax
    push rdi
    push rsi
    push rdx
    call thread_current
    test rax, rax
    jz .skip_save_context

    lea rbx, [rax + THREAD_USER_CTX_OFFSET]

    ;the saved syscall frame is logically a ring 3 frame even though we are on the kernel stack now
    ;so stamp in the fixed user segment selectors here
    mov qword [rbx + CTX_CS], USER_CS
    mov qword [rbx + CTX_SS], USER_DS

    ;callee-saved regs and preserved base pointer
    mov rax, [rbp - 8]
    mov [rbx + CTX_RBX], rax
    mov rax, [rbp + 0]
    mov [rbx + CTX_RBP], rax
    mov rax, [rbp - 16]
    mov [rbx + CTX_R12], rax
    mov rax, [rbp - 24]
    mov [rbx + CTX_R13], rax
    mov rax, [rbp - 32]
    mov [rbx + CTX_R14], rax
    mov rax, [rbp - 40]
    mov [rbx + CTX_R15], rax

    ;RIP, RSP, rflags are not on a normal iret frame here so rebuild them from the values syscall gave us
    ;RCX R11 also have to be restored as general regs not syscall metadata
    mov rax, [rbp + 16]
    mov [rbx + CTX_RIP], rax
    mov [rbx + CTX_RCX], rax
    mov rax, [rbp + 24]
    mov [rbx + CTX_RSP], rax
    mov rax, [rbp + 8]
    mov [rbx + CTX_RFLAGS], rax
    mov [rbx + CTX_R11], rax

    ;caller-saved gprs and syscall args
    mov rax, [rbp - 72]
    mov [rbx + CTX_RAX], rax
    mov rax, [rbp - 80]
    mov [rbx + CTX_RDI], rax
    mov rax, [rbp - 88]
    mov [rbx + CTX_RSI], rax
    mov rax, [rbp - 96]
    mov [rbx + CTX_RDX], rax
    mov rax, [rbp - 48]
    mov [rbx + CTX_R10], rax
    mov rax, [rbp - 56]
    mov [rbx + CTX_R8], rax
    mov rax, [rbp - 64]
    mov [rbx + CTX_R9], rax

.skip_save_context:
    ;drop the temporary scratch saves used only to build user_context
    pop rdx
    pop rsi
    pop rdi
    pop rax

    ;shuffle syscall args to C calling convention
    ;syscall: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
    ;C call:  rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5, [stack]=a6
    push r9                 ;6th arg on stack for C
    mov r9, r8              ;5th arg
    mov r8, r10             ;4th arg
    mov rcx, rdx            ;3rd arg
    mov rdx, rsi            ;2nd arg
    mov rsi, rdi            ;1st arg
    mov rdi, rax            ;syscall number

    sti                     ;enable interrupts for syscall duration
    call syscall_dispatch
    cli                     ;disable interrupts for return sequence

    ;feed the syscall return value through proc_prepare_syscall_return
    ;it may write rax into user_context preserve a restored context for proc_event_return
    ;or redirect user_context to an async event handler
    ;if we have a current thread return through arch_return_to_usermode so the outgoing frame
    ;always comes from the saved context object instead of rcx r11 directly
    mov r12, rax
    call thread_current
    test rax, rax
    jz .legacy_return

    mov r13, rax
    mov rdi, r13
    mov rsi, r12
    call proc_prepare_syscall_return
    lea rdi, [r13 + THREAD_USER_CTX_OFFSET]
    jmp arch_return_to_usermode

.legacy_return:
    ;fallback for very early bootstrap or any case where there is no current thread object yet
    ;there we still have the raw syscall return state on the kernel stack so use classic sysret

    add rsp, 8              ;pop 6th arg

    ;restore syscall argument registers
    pop r9
    pop r8
    pop r10

    ;restore callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx

    mov rsp, rbp
    pop rbp
    pop r11
    pop rcx

    pop rsp                 ;restore user RSP from kernel stack

    ;switch back from kernel GS percpu to user GS TLS
    swapgs

    o64 sysret
