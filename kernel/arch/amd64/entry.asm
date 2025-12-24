;entry from bootloader expects:
;RDI: pointer to boot info structure (DB protocol)
;long mode already enabled
;paging already set up by bootloader

bits 64
section .text.entry

global _start

extern init

_start:
    ;clear direction flag
    cld

    ;set up initial kernel stack
    lea rsp, [rel kernel_stack_top]

    ;call arch-specific init (which then calls kernel_main)
    call init

.halt:
    cli
    hlt
    jmp .halt


section .bss
align 16
kernel_stack_bottom:
    resb 16384
kernel_stack_top:
