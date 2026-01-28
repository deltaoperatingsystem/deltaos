#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/ioapic.h>
#include <arch/amd64/io.h>
#include <arch/amd64/cpu.h>
#include <arch/amd64/interrupts.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <lib/io.h>
#include <lib/string.h>
#include <drivers/serial.h>

static bool apic_available = false;
static bool force_pic_mode = false;
static uint64 apic_base_phys = 0;
static volatile uint32 *apic_base_virt = NULL;

void apic_write(uint32 reg, uint32 val) {
    if (apic_base_virt) {
        apic_base_virt[reg / 4] = val;
        //memory barrier to ensure write is visible on real hardware
        __asm__ volatile ("mfence" ::: "memory");
    }
}

uint32 apic_read(uint32 reg) {
    if (!apic_base_virt) return 0;
    return apic_base_virt[reg / 4];
}

bool apic_is_supported(void) {
    uint32 eax, ebx, ecx, edx;
    arch_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;
}

uint32 apic_get_id(void) {
    return (apic_read(APIC_ID) >> 24) & 0xFF;
}

void apic_set_force_pic(bool force) {
    force_pic_mode = force;
}

bool apic_init(void) {
    serial_write("[apic] Initializing...\n");
    
    //check if PIC mode is forced via command line
    if (force_pic_mode) {
        serial_write("[apic] PIC mode forced via command line, skipping APIC init\n");
        return false;
    }
    
    if (!apic_is_supported()) {
        serial_write("[apic] ERR: APIC not supported\n");
        return false;
    }

    uint64 apic_base_msr = rdmsr(MSR_APIC_BASE);
    
    //check if x2APIC is enabled (bit 10)
    if (apic_base_msr & (1 << 10)) {
        serial_write("[apic] ERR: x2APIC mode is enabled. DeltaOS only supports xAPIC (MMIO).\n");
        //we could try to disable it by clearing bit 10 but it often requires firmware support 
        //to go back to xAPIC safely once enabled
        return false;
    }

    apic_base_phys = apic_base_msr & 0xFFFFFFFFFFFFF000ULL; //standard mask for bits 12-63

    //map APIC registers using HHDM and ensure mapping exists
    apic_base_virt = (volatile uint32 *)P2V(apic_base_phys);
    vmm_kernel_map((uintptr)apic_base_virt, apic_base_phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    //enable APIC globally via MSR
    wrmsr(MSR_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE);
    
    //memory barrier after enabling APIC
    __asm__ volatile ("mfence" ::: "memory");

    //set spurious interrupt vector and enable software
    apic_write(APIC_SPURIOUS, APIC_SPURIOUS_ENABLE | APIC_SPURIOUS_VECTOR);
    
    //read back to ensure it was written (some hardware requires this)
    uint32 spurious_check = apic_read(APIC_SPURIOUS);
    if ((spurious_check & APIC_SPURIOUS_ENABLE) == 0) {
        serial_write("[apic] ERR: Failed to enable APIC spurious vector\n");
        return false;
    }

    uint32 ver = apic_read(APIC_VERSION);
    printf("[apic] Initialized (Phys: 0x%lx, ID: %u, Ver: 0x%x)\n", 
           apic_base_phys, apic_get_id(), ver & 0xFF);
    serial_write("[apic] Local APIC enabled\n");

    apic_available = true;
    
    //initialize IOAPIC for legacy routing if possible
    serial_write("[apic] Initializing IOAPIC...\n");
    if (ioapic_init()) {
        //disable legacy PIC as IOAPIC now handles routing
        pic_disable();
        serial_write("[apic] Legacy PIC disabled\n");
    } else {
        serial_write("[apic] ERR: IOAPIC initialization failed, aborting APIC setup\n");
        //if IOAPIC fails we should really stick to PIC for everything to be safe
        apic_available = false;
        return false;
    }

    return true;
}

void apic_send_eoi(void) {
    if (apic_available) {
        apic_write(APIC_EOI, 0);
    }
}

bool apic_is_enabled(void) {
    return apic_available;
}

void apic_wait_icr_idle(void) {
    while (apic_read(APIC_ICR_LOW) & (1 << 12)) arch_pause();
}

void apic_send_ipi(uint8 apic_id, uint8 vector) {
    apic_wait_icr_idle();
    apic_write(APIC_ICR_HIGH, (uint32)apic_id << 24);
    apic_write(APIC_ICR_LOW, vector | (0 << 8) | (1 << 14)); //fixed, asserted
}

void apic_timer_init(uint32 hz) {
    if (!apic_available) return;

    printf("[apic] Calibrating timer...\n");

    //tell APIC timer to use divider 16
    apic_write(APIC_TIMER_DCR, 0x03);

    //prepare PIT to count down 10ms (100 Hz signal)
    //mode 0: interrupt on terminal count
    outb(0x43, 0x30); 
    outb(0x40, 0x9B); //low byte of 11931
    outb(0x40, 0x2E); //high byte of 11931

    //start APIC timer counting down from max
    apic_write(APIC_TIMER_ICR, 0xFFFFFFFF);

    //poll PIT until it wraps/hits 0
    uint8 lo, hi;
    uint16 count;
    do {
        outb(0x43, 0x00); //latch counter
        lo = inb(0x40);
        hi = inb(0x40);
        count = (uint16)lo | ((uint16)hi << 8);
    } while (count > 10); //wait until it's almost zero

    //read APIC timer remaining count
    uint32 delta = 0xFFFFFFFF - apic_read(APIC_TIMER_CCR);

    //setup periodic timer
    //vector 32 is IRQ 0 handler in DeltaOS
    apic_write(APIC_LVT_TIMER, 32 | (1 << 17)); //periodic mode, vector 32
    apic_write(APIC_TIMER_DCR, 0x03); //divide by 16
    apic_write(APIC_TIMER_ICR, (delta * 100) / hz); //ticks_per_10ms * 100 = per second

    printf("[apic] timer periodic @ %u Hz (ticks per int: %u)\n", hz, (delta * 100) / hz);
}
