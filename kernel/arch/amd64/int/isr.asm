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

;offset of arch_context within thread_t struct
;thread_t: tid(8) + process*(8) + state(4) + pad(4) + obj*(8) + entry(8) + arg(8) = 48
%define THREAD_CTX_OFFSET 48

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
extern thread_current

common_stub:
    ;save ALL general purpose registers
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
    
    ;stack layout now (15 regs = 120 bytes):
    ;[rsp+0]   = r15
    ;[rsp+8]   = r14
    ;[rsp+16]  = r13
    ;[rsp+24]  = r12
    ;[rsp+32]  = r11
    ;[rsp+40]  = r10
    ;[rsp+48]  = r9
    ;[rsp+56]  = r8
    ;[rsp+64]  = rbp
    ;[rsp+72]  = rdi
    ;[rsp+80]  = rsi
    ;[rsp+88]  = rdx
    ;[rsp+96]  = rcx
    ;[rsp+104] = rbx
    ;[rsp+112] = rax
    ;[rsp+120] = vector
    ;[rsp+128] = error code
    ;[rsp+136] = RIP   (iret frame)
    ;[rsp+144] = CS
    ;[rsp+152] = RFLAGS
    ;[rsp+160] = RSP
    ;[rsp+168] = SS
    
    ;check if we came from user mode (CS RPL = 3)
    ;if so then we need to swapgs to get kernel GS for percpu access
    mov rax, [rsp + 144] ;get CS from stack
    and rax, 3 ;check RPL bits
    jz .skip_swapgs_in ;RPL=0 means kernel, skip
    swapgs ;switch from user GS to kernel GS
.skip_swapgs_in:
    
    ;get current thread to save context to
    call thread_current
    test rax, rax
    jz .skip_save           ;no current thread skip save
    
    ;rax = thread_t*, get context pointer
    lea rbx, [rax + THREAD_CTX_OFFSET]
    
    ;save GPRs from stack to context
    mov rcx, [rsp + 0]      ;r15
    mov [rbx + CTX_R15], rcx
    mov rcx, [rsp + 8]      ;r14
    mov [rbx + CTX_R14], rcx
    mov rcx, [rsp + 16]    ;r13
    mov [rbx + CTX_R13], rcx
    mov rcx, [rsp + 24]     ;r12
    mov [rbx + CTX_R12], rcx
    mov rcx, [rsp + 32]     ;r11
    mov [rbx + CTX_R11], rcx
    mov rcx, [rsp + 40]     ;r10
    mov [rbx + CTX_R10], rcx
    mov rcx, [rsp + 48]     ;r9
    mov [rbx + CTX_R9], rcx
    mov rcx, [rsp + 56]     ;r8
    mov [rbx + CTX_R8], rcx
    mov rcx, [rsp + 64]     ;rbp
    mov [rbx + CTX_RBP], rcx
    mov rcx, [rsp + 72]     ;rdi
    mov [rbx + CTX_RDI], rcx
    mov rcx, [rsp + 80]     ;rsi
    mov [rbx + CTX_RSI], rcx
    mov rcx, [rsp + 88]     ;rdx
    mov [rbx + CTX_RDX], rcx
    mov rcx, [rsp + 96]     ;rcx
    mov [rbx + CTX_RCX], rcx
    mov rcx, [rsp + 104]    ;rbx
    mov [rbx + CTX_RBX], rcx
    mov rcx, [rsp + 112]    ;rax
    mov [rbx + CTX_RAX], rcx
    
    ;save iret frame
    mov rcx, [rsp + 136]    ;RIP
    mov [rbx + CTX_RIP], rcx
    mov rcx, [rsp + 144]    ;CS
    mov [rbx + CTX_CS], rcx
    mov rcx, [rsp + 152]    ;RFLAGS
    mov [rbx + CTX_RFLAGS], rcx
    mov rcx, [rsp + 160]    ;RSP
    mov [rbx + CTX_RSP], rcx
    mov rcx, [rsp + 168]    ;SS
    mov [rbx + CTX_SS], rcx

.skip_save:
    ;call interrupt handler
    mov rdi, [rsp + 120] ;vector
    mov rsi, [rsp + 128] ;error code
    mov rdx, [rsp + 136] ;RIP
    call interrupt_handler
    
    ;get (POSSSIBLYYY new) current thread to restore from
    call thread_current
    test rax, rax
    jz .restore_from_stack  ;no thread\ restore from stack as-is
    
    lea rbx, [rax + THREAD_CTX_OFFSET]
    
    ;restore iret frame from context to stack
    mov rcx, [rbx + CTX_SS]
    mov [rsp + 168], rcx
    mov rcx, [rbx + CTX_RSP]
    mov [rsp + 160], rcx
    mov rcx, [rbx + CTX_RFLAGS]
    mov [rsp + 152], rcx
    mov rcx, [rbx + CTX_CS]
    mov [rsp + 144], rcx
    mov rcx, [rbx + CTX_RIP]
    mov [rsp + 136], rcx
    
    ;restore GPRs from context to stack
    mov rcx, [rbx + CTX_RAX]
    mov [rsp + 112], rcx
    mov rcx, [rbx + CTX_RBX]
    mov [rsp + 104], rcx
    mov rcx, [rbx + CTX_RCX]
    mov [rsp + 96], rcx
    mov rcx, [rbx + CTX_RDX]
    mov [rsp + 88], rcx
    mov rcx, [rbx + CTX_RSI]
    mov [rsp + 80], rcx
    mov rcx, [rbx + CTX_RDI]
    mov [rsp + 72], rcx
    mov rcx, [rbx + CTX_RBP]
    mov [rsp + 64], rcx
    mov rcx, [rbx + CTX_R8]
    mov [rsp + 56], rcx
    mov rcx, [rbx + CTX_R9]
    mov [rsp + 48], rcx
    mov rcx, [rbx + CTX_R10]
    mov [rsp + 40], rcx
    mov rcx, [rbx + CTX_R11]
    mov [rsp + 32], rcx
    mov rcx, [rbx + CTX_R12]
    mov [rsp + 24], rcx
    mov rcx, [rbx + CTX_R13]
    mov [rsp + 16], rcx
    mov rcx, [rbx + CTX_R14]
    mov [rsp + 8], rcx
    mov rcx, [rbx + CTX_R15]
    mov [rsp + 0], rcx

.restore_from_stack:
    ;restore all GPRs
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
    
    ;remove vector and error code
    add rsp, 16
    
    ;check if returning to user mode (CS RPL = 3)
    ;if so, we need to swapgs back to user GS
    ;stack layout now: [rsp+0]=RIP, [rsp+8]=CS, [rsp+16]=RFLAGS, [rsp+24]=RSP, [rsp+32]=SS
    test qword [rsp + 8], 3 ;check RPL bits directly
    jz .skip_swapgs_out ;RPL=0 means returning to kernel, skip
    swapgs ;switch from kernel GS to user GS
.skip_swapgs_out:
    
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