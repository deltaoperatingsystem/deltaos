bits 64
section .text
global _start

;syscall numbers
%define SYS_exit        0
%define SYS_getpid      1
%define SYS_yield       2
%define SYS_debug_write 3
%define SYS_write       4

_start:
    ;write message to VT console
    mov rax, SYS_write
    lea rdi, [rel msg_hello]
    mov rsi, msg_hello_len
    syscall
    
    ;get our PID
    mov rax, SYS_getpid
    syscall
    ;result in rax
    
    ;write done message
    mov rax, SYS_write
    lea rdi, [rel msg_done]
    mov rsi, msg_done_len
    syscall
    
    ;exit
    mov rax, SYS_exit
    xor rdi, rdi
    syscall
    
    ;should never reach here
    hlt
    jmp $

section .rodata
msg_hello: db "[user] hello from userspace!", 10
msg_hello_len equ $ - msg_hello

msg_done: db "[user] syscall test complete, exiting", 10
msg_done_len equ $ - msg_done
