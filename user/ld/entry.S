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
