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
    
    push rcx
    push r11
    push rbp
    mov rbp, rsp
    
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    push r9
    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax
    
    call syscall_dispatch
    
    add rsp, 8
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    
    mov rsp, rbp
    pop rbp
    pop r11
    pop rcx
    
    mov rsp, [gs:PERCPU_USER_RSP]
    
    swapgs
    
    o64 sysret
