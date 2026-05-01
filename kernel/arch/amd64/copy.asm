section .text
global mmu_copy_to_user
global mmu_copy_from_user

;int mmu_copy_to_user(void *dst, const void *src, size_t len, volatile uintptr_t *recovery_ptr, uintptr_t recovery_addr)
;rdi = dst, rsi = src, rdx = len, rcx = recovery_ptr, r8 = recovery_addr
mmu_copy_to_user:
    test rdx, rdx
    jz .to_success
    
    mov [rcx], r8           ;set recovery_rip
    
.to_loop:
    mov al, [rsi]           ;read from kernel
    mov [rdi], al           ;write to user (might fault)
    inc rsi
    inc rdi
    dec rdx
    jnz .to_loop

    mov qword [rcx], 0      ;clear recovery_rip
.to_success:
    xor rax, rax
    ret

;int mmu_copy_from_user(void *dst, const void *src, size_t len, volatile uintptr_t *recovery_ptr, uintptr_t recovery_addr)
;rdi = dst, rsi = src, rdx = len, rcx = recovery_ptr, r8 = recovery_addr
mmu_copy_from_user:
    test rdx, rdx
    jz .from_success
    
    mov [rcx], r8           ;set recovery_rip

.from_loop:
    mov al, [rsi]           ;read from user (might fault)
    mov [rdi], al           ;write to kernel
    inc rsi
    inc rdi
    dec rdx
    jnz .from_loop

    mov qword [rcx], 0      ;clear recovery_rip
.from_success:
    xor rax, rax
    ret

;helper for syscall recovery
global mmu_user_access_fault
mmu_user_access_fault:
    xor rax, rax
    inc rax                 ;return 1 (error)
    ret
