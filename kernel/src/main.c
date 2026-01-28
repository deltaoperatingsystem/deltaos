#include <arch/types.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <arch/interrupts.h>
#include <arch/mmu.h>
#include <drivers/fb.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/rtc.h>
#include <drivers/nvme.h>
#include <drivers/serial.h>
#include <drivers/vt/vt.h>
#include <lib/string.h>
#include <lib/io.h>
#include <boot/db.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <obj/handle.h>
#include <obj/namespace.h>
#include <obj/rights.h>
#include <proc/process.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <fs/tmpfs.h>
#include <fs/initrd.h>
#include <kernel/elf64.h>

extern void arch_enter_usermode(arch_context_t *ctx);
extern void percpu_set_kernel_stack(void *stack_top);


//load and execute init from initrd
static void spawn_init(void) {
    //open init
    handle_t h = handle_open("$files/system/binaries/init", HANDLE_RIGHT_READ);
    if (h == INVALID_HANDLE) {
        printf("[init] failed to open /system/binaries/init\n");
        return;
    }
    
    //allocate buffer for init binary
    size buf_size = 32768;  //32KB should be enough
    char *buf = kzalloc(buf_size);
    if (!buf) {
        printf("[init] failed to allocate buffer\n");
        return;
    }
    
    ssize len = handle_read(h, buf, buf_size);
    handle_close(h);
    
    if (len <= 0) {
        printf("[init] failed to read init binary\n");
        kfree(buf);
        return;
    }
    printf("[init] loaded init: %ld bytes\n", len);
    
    //validate ELF
    if (!elf_validate(buf, len)) {
        printf("[init] invalid ELF\n");
        kfree(buf);
        return;
    }
    
    //create user process
    process_t *proc = process_create_user("init");
    if (!proc) {
        printf("[init] failed to create process\n");
        kfree(buf);
        return;
    }
    printf("[init] created process PID %lu\n", proc->pid);
    
    //load ELF into user address space
    elf_load_info_t info;
    int err = elf_load_user(buf, len, proc, &info);
    if (err != ELF_OK) {
        printf("[init] ELF load failed: %d\n", err);
        process_destroy(proc);
        kfree(buf);
        return;
    }
    printf("[init] entry: 0x%lX\n", info.entry);
    printf("[init] loaded %u segments:\n", info.segment_count);
    for (uint32 i = 0; i < info.segment_count; i++) {
        printf("[init]   seg %u: virt=0x%lX phys=0x%lX pages=%lu\n",
               i, info.segments[i].virt_addr, info.segments[i].phys_addr, info.segments[i].pages);
    }
    
    //check for dynamic executable (has interpreter)
    uint64 interp_base = 0;
    uint64 real_entry = info.entry;
    
    if (info.interp_path[0]) {
        printf("[init] dynamic executable, interpreter: %s\n", info.interp_path);
        
        //convert interpreter path to full path
        //e.x /system/libraries/ld.so -> $files/system/libraries/ld.so
        char interp_fullpath[256];
        if (info.interp_path[0] == '/') {
            //absolute path - prepend $files
            snprintf(interp_fullpath, sizeof(interp_fullpath), "$files%s", info.interp_path);
        } else {
            snprintf(interp_fullpath, sizeof(interp_fullpath), "$files/%s", info.interp_path);
        }
        
        //load interpreter
        handle_t ih = handle_open(interp_fullpath, HANDLE_RIGHT_READ);
        if (ih == INVALID_HANDLE) {
            printf("[init] failed to open interpreter: %s\n", interp_fullpath);
            process_destroy(proc);
            kfree(buf);
            return;
        }
        
        size interp_buf_size = 32768; //32KB for interpreter
        char *interp_buf = kzalloc(interp_buf_size);
        if (!interp_buf) {
            printf("[init] failed to allocate interpreter buffer\n");
            handle_close(ih);
            process_destroy(proc);
            kfree(buf);
            return;
        }

        ssize interp_len = handle_read(ih, interp_buf, interp_buf_size);
        handle_close(ih);
        printf("[init] interpreter file: %ld bytes read\n", interp_len);
        
        if (interp_len <= 0 || !elf_validate(interp_buf, interp_len)) {
            printf("[init] invalid interpreter ELF\n");
            process_destroy(proc);
            kfree(interp_buf);
            kfree(buf);
            return;
        }
        
        //load interpreter into address space
        elf_load_info_t interp_info;
        err = elf_load_user(interp_buf, interp_len, proc, &interp_info);
        if (err != ELF_OK) {
            printf("[init] failed to load interpreter: %d\n", err);
            process_destroy(proc);
            kfree(interp_buf);
            kfree(buf);
            return;
        }
        
        interp_base = interp_info.virt_base;
        real_entry = interp_info.entry;  //jump to interpreter not executable
        printf("[init] interpreter loaded at 0x%lX, entry 0x%lX\n", interp_base, real_entry);
        kfree(interp_buf);
        printf("[init] interp %u segments:\n", interp_info.segment_count);
        for (uint32 i = 0; i < interp_info.segment_count; i++) {
            printf("[init]   seg %u: virt=0x%lX phys=0x%lX pages=%lu\n",
                   i, interp_info.segments[i].virt_addr, interp_info.segments[i].phys_addr, 
                   interp_info.segments[i].pages);
        }
    }
    
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
    
    //track stack in VMA list
    process_vma_add(proc, user_stack_base - stack_size, stack_size,
                    MMU_FLAG_WRITE | MMU_FLAG_USER, NULL, 0);
    
    //set up argc/argv and aux vector
    char *init_argv[] = { "/system/binaries/init", NULL };
    int init_argc = sizeof(init_argv) / sizeof(init_argv[0]) - 1;
    
    uintptr user_stack_top;
    if (info.interp_path[0]) {
        //dynamic executable: use aux vector stack setup
        user_stack_top = process_setup_user_stack_dynamic(
            stack_phys, user_stack_base, stack_size, init_argc, init_argv,
            info.phdr_addr, info.phdr_count, info.phdr_size,
            info.entry,  //AT_ENTRY = original program entry
            interp_base  //AT_BASE = interpreter load address
        );
    } else {
        //static executable: simple stack setup
        user_stack_top = process_setup_user_stack(stack_phys, user_stack_base, 
                                                   stack_size, init_argc, init_argv);
    }
    printf("[init] stack at 0x%lX, argc=1, argv[0]=%s\n", user_stack_top, init_argv[0]);
    
    //create user thread
    thread_t *thread = thread_create_user(proc, (void*)real_entry, (void*)user_stack_top);
    if (!thread) {
        printf("[init] failed to create thread\n");
        kfree(buf);
        return;
    }
    printf("[init] created thread TID %lu\n", thread->tid);
    
    //add init thread to scheduler
    sched_add(thread);
    
    //free the init buffer now that it's loaded
    kfree(buf);
}

void parse_cmdline(const char *cmdline) {
    if (!cmdline) return;
    char buf[256];
    size len = strlen(cmdline);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, cmdline, len);
    buf[len] = '\0';
    char *arg = strtok(buf, " ");
    while (arg) {
        if (strcmp(arg, "debug") == 0) io_enable_serial();
        arg = strtok(NULL, " ");
    }
}

void kernel_main(const char *cmdline) {
    parse_cmdline(cmdline);
    set_outmode(SERIAL);
    printf("kernel_main started\n");
    
    ns_register("$devices", ns_create_dir("$devices/"));
    
    //initialize drivers
    fb_init();
    fb_init_backbuffer();
    con_init();
    vt_init();
    keyboard_init();
    mouse_init();
    nvme_init();
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
