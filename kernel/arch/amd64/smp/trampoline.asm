section .rodata

global _trampoline_start
global _trampoline_end

_trampoline_start:
    incbin "arch/amd64/smp/trampoline.bin"
_trampoline_end:
