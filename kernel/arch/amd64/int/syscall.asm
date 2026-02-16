bits 64
section .text

%define PERCPU_KERNEL_RSP   0
%define PERCPU_USER_RSP     8

extern syscall_dispatch

global syscall_entry_simple
syscall_entry_simple:
    swapgs
    mov [gs:PERCPU_USER_RSP], rsp
    mov rsp, [gs:PERCPU_KERNEL_RSP]
    
    ;save registers clobbered by syscall instruction
    push qword [gs:PERCPU_USER_RSP] ;save REAL user RSP on kernel stack
    push rcx                ;RIP saved by syscall
    push r11                ;RFLAGS saved by syscall
    push rbp
    mov rbp, rsp
    
    ;save callee-saved registers
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ;save syscall argument registers (caller-saved but user expects them preserved)
    push r10
    push r8
    push r9
    
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
    
    swapgs
    
    o64 sysret
