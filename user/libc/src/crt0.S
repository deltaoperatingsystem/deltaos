/*
 *stack layout on entry (System V AMD64 ABI):
 *(%rsp)     = argc
 *8(%rsp)    = argv[0]
 *16(%rsp)   = argv[1]
 *...
 *8+8*argc   = NULL (argv terminator)
 */

.global _start
.section .text

_start:
    /*clear frame pointer for clean backtraces*/
    xor %rbp, %rbp

    /*Load argc and argv*/
    mov (%rsp), %rdi        /*argc -> first arg*/
    lea 8(%rsp), %rsi       /*argv -> second arg*/

    /*align stack to 16-byte boundary*/
    and $-16, %rsp

    /*save argc/argv across _lib_init call*/
    push %rsi
    push %rdi

    /*initialize the library*/
    call _lib_init

    /*restore argc/argv*/
    pop %rdi
    pop %rsi

    /*call main(argc, argv)*/
    call main

    /*main returned - exit with return value*/
    mov %rax, %rdi
    call exit

    /*should never reach here= but just in case*/
1:
    hlt
    jmp 1b
