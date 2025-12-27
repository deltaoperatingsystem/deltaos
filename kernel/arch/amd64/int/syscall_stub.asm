bits 64
global syscall_entry_stub

extern syscall_handler
syscall_entry_stub:
    call syscall_handler