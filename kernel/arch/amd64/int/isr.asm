%macro isr_err_stub 1
isr_stub_%+%1:
    push qword %1       ;push vector (error code already on stack from CPU)
    jmp common_stub
%endmacro

%macro isr_no_err_stub 1
isr_stub_%+%1:
    push qword 0        ;push dummy error code
    push qword %1       ;push vector
    jmp common_stub
%endmacro

extern interrupt_handler

common_stub:
    ;save all caller-saved registers
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    ;stack layout after pushes:
    ;[rsp + 72] = vector
    ;[rsp + 80] = error code
    mov rdi, [rsp + 72] ;vector -> first arg
    mov rsi, [rsp + 80] ;error code -> second arg
    call interrupt_handler

    ;restore registers
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    ;remove vector and error code from stack
    add rsp, 16

    ;return from interrupt
    iretq

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

;IRQs and software interrupts (32-255)
;none of these push error codes
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