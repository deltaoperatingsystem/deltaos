#include <arch/types.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <arch/interrupts.h>
#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/rtc.h>
#include <drivers/serial.h>
#include <drivers/vt/vt.h>
#include <lib/string.h>
#include <lib/io.h>
#include <boot/db.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <obj/handle.h>
#include <obj/rights.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <fs/tmpfs.h>
#include <fs/initrd.h>
#include <lib/string.h>
#include <proc/sched.h>
#include <arch/mmu.h>
#include <kernel/elf64.h>

extern void arch_enter_usermode(arch_context_t *ctx);
    extern void percpu_set_kernel_stack(void *stack_top);


//load and execute init from initrd
static void spawn_init(void) {
    //open init
    handle_t h = handle_open("$files/initrd/init", HANDLE_RIGHT_READ);
    if (h == INVALID_HANDLE) {
        printf("[init] failed to open /initrd/init\n");
        return;
    }
    
    //read init binary
    char buf[8192];
    ssize len = handle_read(h, buf, sizeof(buf));
    handle_close(h);
    
    if (len <= 0) {
        printf("[init] failed to read init binary\n");
        return;
    }
    printf("[init] loaded init: %ld bytes\n", len);
    
    //validate ELF
    if (!elf_validate(buf, len)) {
        printf("[init] invalid ELF\n");
        return;
    }
    
    //create user process
    process_t *proc = process_create_user("init");
    if (!proc) {
        printf("[init] failed to create process\n");
        return;
    }
    printf("[init] created process PID %lu\n", proc->pid);
    
    //load ELF into user address space
    elf_load_info_t info;
    int err = elf_load_user(buf, len, proc->pagemap, &info);
    if (err != ELF_OK) {
        printf("[init] ELF load failed: %d\n", err);
        process_destroy(proc);
        return;
    }
    printf("[init] entry: 0x%lx\n", info.entry);
    
    //allocate user stack
    uintptr user_stack_base = 0x7FFFFFFFE000ULL;
    size stack_size = 0x2000;
    
    uintptr stack_phys = (uintptr)pmm_alloc(stack_size / 4096);
    if (!stack_phys) {
        printf("[init] failed to allocate stack\n");
        return;
    }
    mmu_map_range(proc->pagemap, user_stack_base - stack_size, stack_phys, 
                  stack_size / 4096, MMU_FLAG_WRITE | MMU_FLAG_USER);
    
    //set up argc/argv on user stack
    //we need to write to the physical page since user pagemap isn't active
    //stack layout (growing down):
    //[argv strings]
    //[padding for alignment]
    //[NULL] <- argv terminator
    //[argv[0] ptr]
    //[argc] <- RSP points here on entry
    
    const char *argv0 = "/initrd/init";
    size argv0_len = strlen(argv0) + 1;
    
    //calculate user stack pointer positions
    uintptr sp = user_stack_base;
    
    //write argv[0] string at top of stack
    sp -= argv0_len;
    sp &= ~7ULL;  //align to 8 bytes
    uintptr argv0_user_addr = sp;
    
    //write to physical memory (offset from stack_phys)
    char *stack_virt = (char *)P2V(stack_phys);
    size offset_from_base = user_stack_base - stack_size;
    memcpy(stack_virt + (argv0_user_addr - offset_from_base), argv0, argv0_len);
    
    //now push the argv array and argc
    //need: argc, argv[0], NULL -> 3 * 8 = 24 bytes
    //for 16-byte alignment before call: (argc + argv_count + 1) should be even
    //we have argc=1, so 1 + 1 + 1 = 3 pointers, need padding for 16-byte align
    sp -= 8;  //NULL terminator for argv
    uint64 *slot = (uint64 *)(stack_virt + (sp - offset_from_base));
    *slot = 0;
    
    sp -= 8;  //argv[0]
    slot = (uint64 *)(stack_virt + (sp - offset_from_base));
    *slot = argv0_user_addr;
    
    sp -= 8;  //argc
    slot = (uint64 *)(stack_virt + (sp - offset_from_base));
    *slot = 1;  //argc = 1
    
    uintptr user_stack_top = sp;
    printf("[init] stack at 0x%lx, argc=1, argv[0]=%s\n", user_stack_top, argv0);
    
    //create user thread
    thread_t *thread = thread_create_user(proc, (void*)info.entry, (void*)user_stack_top);
    if (!thread) {
        printf("[init] failed to create thread\n");
        return;
    }
    printf("[init] created thread TID %lu\n", thread->tid);
    
    //set up per-CPU kernel stack for syscalls
    percpu_set_kernel_stack((char*)thread->kernel_stack + thread->kernel_stack_size);
    
    //add init thread to scheduler
    sched_add(thread);
}

void kernel_main(void) {
    set_outmode(SERIAL);
    printf("kernel_main started\n");
    
    //initialize drivers
    fb_init();
    fb_init_backbuffer();
    con_init();
    vt_init();
    keyboard_init();
    serial_init_object();
    rtc_init();
    
    //initialize filesystems
    tmpfs_init();
    initrd_init();
    
    //initialize scheduler (creates idle thread)
    sched_init();
    syscall_init();
    
    //spawn init process
    spawn_init();
    
    //start scheduler - never returns
    printf("[kernel] starting scheduler...\n");
    sched_start();
    
    //should never reach here
    printf("[kernel] ERROR: scheduler returned!\n");
    for (;;) arch_halt();
}
