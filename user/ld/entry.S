.section .data
.global __ld_saved_sp
__ld_saved_sp:
    .quad 0

.section .text
.global _start
.extern ld_main

_start:
    #save RSP immediately to memory (ain't trusting registers after C code runs)
    mov %rsp, __ld_saved_sp(%rip)
    
    #align stack and call ld_main
    mov %rsp, %rdi          #pass original SP as argument
    and $~0xF, %rsp         #16-byte align stack
    xor %rbp, %rbp          #clear frame pointer
    call ld_main
    
    #RAX = entrys point (or 0 on error)
    test %rax, %rax
    jz .Lerror
    
    #restore original stack and jump to entry point
    mov __ld_saved_sp(%rip), %rsp
    xor %rbp, %rbp
    jmp *%rax

.Lerror:
    #exit with code 1
    mov $0, %rax            #SYS_EXIT
    mov $1, %rdi            #exit code
    syscall
    hlt

.global _ld_runtime_resolve_asm
.extern ld_runtime_resolve
_ld_runtime_resolve_asm:
    #save caller-saved registers (9 total)
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    #entry RSP was 8-aligned, 9 pushes -> 72 bytes. 8 - 72 = -64 aligned

    #lib is at [RSP + 9*8], index is at [RSP + 10*8]
    mov 9*8(%rsp), %rdi    
    mov 10*8(%rsp), %rsi   

    call ld_runtime_resolve

    #put target address into saved R11 slot
    mov %rax, (%rsp)

    #restore registers R11 will contain the target address
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax

    #clean up PLT arguments
    add $16, %rsp

    #jump to target
    jmp *%r11
