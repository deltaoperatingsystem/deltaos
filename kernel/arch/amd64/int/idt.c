#include <arch/amd64/types.h>
#include <arch/amd64/interrupts.h>
#include <arch/amd64/timer.h>
#include <arch/mmu.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/rtl8139.h>
#include <drivers/vt/vt.h>
#include <drivers/usb/xhci.h>
#include <lib/io.h>
#include <arch/amd64/context.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/ioapic.h>
#include <arch/percpu.h>
#include <arch/smp.h>
#include <mm/kheap.h>
#include <proc/sched.h>
#include <proc/process.h>

struct idt_entry {
	uint16    isr_low;      // The lower 16 bits of the ISR's address
	uint16    kernel_cs;    // The GDT segment selector that the CPU will load into CS before calling the ISR
	uint8	  ist;          // The IST in the TSS that the CPU will load into RSP; set to zero for now
	uint8     attributes;   // Type and attributes; see the IDT page
	uint16    isr_mid;      // The higher 16 bits of the lower 32 bits of the ISR's address
	uint32    isr_high;     // The higher 32 bits of the ISR's address
	uint32    reserved;     // Set to zero
} __attribute__((packed));

#define IDT_MAX_DESCRIPTORS 256

__attribute__((aligned(0x10)))
static struct idt_entry idt[IDT_MAX_DESCRIPTORS];

struct idtr  {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

static struct idtr idtr;
extern void *isr_stub_table[];
extern void arch_timer_tick(void);

static inline uintptr arch_read_cr2(void) {
    uintptr cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static void pf_print_flags(uint64 err) {
    char buf[128];
    size pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "  Flags:");
    pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", (err & 1) ? "P" : "NP");
    pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", (err & 2) ? "W" : "R");
    pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", (err & 4) ? "U" : "K");
    if (err & 8) pos += snprintf(buf + pos, sizeof(buf) - pos, " RSVD");
    if (err & 16) pos += snprintf(buf + pos, sizeof(buf) - pos, " IFETCH");
    if (err & 32) pos += snprintf(buf + pos, sizeof(buf) - pos, " PK");
    if (err & 64) pos += snprintf(buf + pos, sizeof(buf) - pos, " SS");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");

    debug_write(buf, pos);
}

static void pf_log_user_fault(interrupt_frame_t *frame, uint64 error_code) {
    char buf[256];
    uintptr cr2 = arch_read_cr2();
    process_t *p = process_current();

    debug_write("\n*** USER PAGE FAULT ***\n", 24);

    if (p) {
        int n = snprintf(buf, sizeof(buf), "  Process:   pid=%u name=%s\n", p->pid, p->name);
        debug_write(buf, n);
    }

    int n = snprintf(buf, sizeof(buf), "  Vector:    0x%lX\n", frame->vector);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "  Error:     0x%lX\n", error_code);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "  CR2:       0x%lX\n", cr2);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "  RIP:       0x%lX (CS: 0x%lX)\n", frame->rip, frame->cs);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "  RSP:       0x%lX (SS: 0x%lX)\n", frame->rsp, frame->ss);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "  RFLAGS:    0x%lX\n", frame->rflags);
    debug_write(buf, n);

    pf_print_flags(error_code);

    debug_write("  Registers:\n", 13);
    n = snprintf(buf, sizeof(buf), "    RAX=0x%016lx RBX=0x%016lx RCX=0x%016lx\n", frame->rax, frame->rbx, frame->rcx);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "    RDX=0x%016lx RSI=0x%016lx RDI=0x%016lx\n", frame->rdx, frame->rsi, frame->rdi);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "    RBP=0x%016lx R8 =0x%016lx R9 =0x%016lx\n", frame->rbp, frame->r8, frame->r9);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "    R10=0x%016lx R11=0x%016lx R12=0x%016lx\n", frame->r10, frame->r11, frame->r12);
    debug_write(buf, n);
    n = snprintf(buf, sizeof(buf), "    R13=0x%016lx R14=0x%016lx R15=0x%016lx\n", frame->r13, frame->r14, frame->r15);
    debug_write(buf, n);
}

typedef struct irq_handler_node {
    void (*handler)(void);
    struct irq_handler_node *next;
} irq_handler_node_t;

static irq_handler_node_t *irq_handlers[16] = {0};
static spinlock_irq_t irq_handler_lock = SPINLOCK_IRQ_INIT;

static bool irq_dispatch_handlers(uint8 irq) {
    if (irq >= 16) return false;

    void (*handlers[16])(void);
    int handler_count = 0;

    irq_state_t flags = spinlock_irq_acquire(&irq_handler_lock);
    irq_handler_node_t *node = irq_handlers[irq];
    while (node && handler_count < 16) {
        if (node->handler) {
            handlers[handler_count++] = node->handler;
        }
        node = node->next;
    }
    spinlock_irq_release(&irq_handler_lock, flags);

    bool handled = false;
    for (int i = 0; i < handler_count; i++) {
        handlers[i]();
        handled = true;
    }
    return handled;
}

int interrupt_register(uint8 irq, void (*handler)(void)) {
    if (irq >= 16 || !handler) return -1;

    irq_handler_node_t *node = kzalloc(sizeof(*node));
    if (!node) return -1;
    node->handler = handler;

    irq_state_t flags = spinlock_irq_acquire(&irq_handler_lock);
    irq_handler_node_t *tail = NULL;
    for (irq_handler_node_t *cur = irq_handlers[irq]; cur; cur = cur->next) {
        if (cur->handler == handler) {
            spinlock_irq_release(&irq_handler_lock, flags);
            kfree(node);
            return -1;
        }
        tail = cur;
    }

    if (tail) tail->next = node;
    else irq_handlers[irq] = node;

    spinlock_irq_release(&irq_handler_lock, flags);
    return 0;
}

static void irq0_handler(int from_usermode) {
    arch_timer_tick();
    if (percpu_get()->cpu_index == 0) {
        vt_tick();
    }
    sched_tick(from_usermode);  //preemptive scheduling - only preempt if from usermode
}

void interrupt_handler(uint64 vector, uint64 error_code, uint64 rip, interrupt_frame_t *frame) {
    if (vector < 32) {
        //check for safe-copy recovery
        if (vector == PAGE_FAULT_VECTOR) {
            percpu_t *cpu = percpu_get();
            if (cpu->recovery_rip != 0) {
                frame->rip = cpu->recovery_rip;
                cpu->recovery_rip = 0;
                return;
            }
        }

        // TODO: this is a basic impl
        // we should probably have our own POSIX signal equivalent
        process_t *p = process_current();
        if (p) {
            if (vector == PAGE_FAULT_VECTOR) {
                pf_log_user_fault(frame, error_code);
            } else {
                char buf[192];
                int n = snprintf(buf, sizeof(buf),
                                 "\n*** USER EXCEPTION *** vector=0x%lX err=0x%lX rip=0x%lX pid=%u name=%s\n",
                                 vector, error_code, rip, p->pid, p->name);
                debug_write(buf, n);
            }
            p->exit_code = 127 + vector;
            thread_exit();
            return;
        } else {
            kpanic(frame, "CPU EXCEPTION (vector 0x%lX, err 0x%lX) at RIP=0x%lX", vector, error_code, rip);
        }
    } else {
        uint8 irq = vector - 32;

        if (apic_is_enabled() && ioapic_is_enabled()) {
            apic_send_eoi();
        } else {
            pic_send_eoi(irq);
        }
        
        //check if we were interrupted from usermode using CS.RPL (authoritative)
        int from_usermode = ((frame->cs & 3) == 3) ? 1 : 0;
        
        if (vector == IPI_RESCHEDULE) {
            //if interrupted from usermode, use sched_preempt() which updates the current-thread pointer
            //the ISRs user_return path then loads the new thread's context cleanly
            if (from_usermode) {
                sched_preempt();
            }
            return;
        }

        bool handled = false;
        switch (irq) {
            case 0:
                irq0_handler(from_usermode);
                handled = true;
                break;
            case 1:
                keyboard_irq();
                handled = true;
                break;
            case 12:
                mouse_irq();
                handled = true;
                break;
            default:
                break;
        }

        if (irq_dispatch_handlers(irq)) {
            handled = true;
        }

        if (vector >= 0x40 && vector <= 0x47) {
            extern void nvme_isr_callback(uint64);
            nvme_isr_callback(vector);
            return;
        } else if (vector == XHCI_MSI_VECTOR) {
            xhci_irq();
            return;
        }

        if (!handled && irq < 16) {
            printf("Unhandled IRQ: 0x%X (vector 0x%X)\n", irq + 32, vector);
        }

        return;
    }
}

void interrupt_mask(uint8 irq) {
    if (apic_is_enabled() && ioapic_is_enabled()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
    }
}

void interrupt_unmask(uint8 irq) {
    if (apic_is_enabled() && ioapic_is_enabled()) {
        ioapic_unmask_irq(irq);
    } else {
        pic_clear_mask(irq);
    }
}

static void idt_setgate(uint8 vector, void *isr, uint8 flags, uint8 ist) {
    struct idt_entry *gate = &idt[vector];

    gate->isr_low = (uint64)isr & 0xFFFF;
    gate->kernel_cs = GDT_KERNEL_CODE;
    gate->ist = ist;  //IST index (1-7) or 0 for no IST
    gate->attributes = flags;
    gate->isr_mid = ((uint64)isr >> 16) & 0xFFFF;
    gate->isr_high = ((uint64)isr >> 32) & 0xFFFFFFFF;
    gate->reserved = 0;
}

void arch_interrupts_init(void) {
    gdt_init();
    idtr.base = (uintptr)&idt[0];
    idtr.limit = (uint16)sizeof(idt) - 1;

    //install handlers for all 256 vectors.
    //we avoid using IST for general interrupts to prevent "leaking" execution onto the shared IST stack
    //IST is only used for critical exceptions like double fault (vector 8)
    for (int vector = 0; vector < 256; vector++) {
        uint8 ist = (vector == 8) ? 1 : 0;
        idt_setgate(vector, isr_stub_table[vector], 0x8E, ist);
    }

    //remap PIC IRQ0-7 -> vectors 32-39, IRQ8-15 -> vectors 40-47
    pic_remap(0x20, 0x28);

    //try to initialize APIC
    if (apic_init()) {
        printf("[amd64] APIC/IOAPIC initialized\n");
    } else {
        //fallback to PIC
        interrupt_unmask(2); //unmask cascade IRQ 2 for slave PIC
        printf("[amd64] Using legacy PIC\n");
    }

    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void arch_interrupts_enable(void) {
    __asm__ volatile ("sti");
}

void arch_interrupts_disable(void) {
    __asm__ volatile ("cli");
}

void idt_load(void) {
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void *idt_get_ptr(void) {
    return &idtr;
}
