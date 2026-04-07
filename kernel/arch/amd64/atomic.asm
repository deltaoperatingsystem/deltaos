global atomic_signal_fence

section .text

; AMD64 fence implementation.
; mfence orders prior loads/stores before subsequent loads/stores.
atomic_signal_fence:
    mfence
    ret
