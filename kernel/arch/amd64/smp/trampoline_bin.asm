[ORG 0x8000]
[BITS 16]

trampoline_start:
    cli
    cld
    
    ;set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7000
    
    ;load 32-bit GDT
    lgdt [gdt32_ptr]
    
    ;enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    
    ;far jump to protected mode
    jmp 0x08:trampoline_32

[BITS 32]
trampoline_32:
    ;set up segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ;enable PAE (required for long mode)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    
    ;load CR3 from trampoline data (0x9000 + 8)
    mov eax, [0x9008]
    mov cr3, eax
    
    ;enable long mode and NX in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)   ;set LME (bit 8) and NXE (bit 11)
    wrmsr
    
    ;load 64-bit GDT
    lgdt [gdt64_ptr]
    
    ;enable paging - this activates long mode
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    
    ;far jump to 64-bit long mode
    jmp 0x08:trampoline_64

[BITS 64]
trampoline_64:
    ;set up segments for 64-bit mode
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    
    ;load stack from 0x9000
    mov rsp, [0x9000]
    
    ;load entry point and cpu_index
    mov rax, [0x9010]
    mov edi, [0x9018]
    
    ;jump to C entry
    jmp rax

ALIGN 16
gdt32:
    dq 0                          ;null
    dq 0x00CF9A000000FFFF         ;32-bit code
    dq 0x00CF92000000FFFF         ;32-bit data
gdt32_end:

gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32

ALIGN 16
gdt64:
    dq 0                          ;null
    dq 0x00AF9A000000FFFF         ;64-bit code
    dq 0x00CF92000000FFFF         ;64-bit data
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

trampoline_end:
