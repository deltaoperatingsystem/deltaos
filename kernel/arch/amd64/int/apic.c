#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/ioapic.h>
#include <arch/amd64/int/iommu.h>
#include <arch/amd64/io.h>
#include <arch/amd64/cpu.h>
#include <arch/amd64/interrupts.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <lib/io.h>
#include <lib/string.h>
#include <drivers/serial.h>
#include <arch/amd64/acpi/acpi.h>
#include <arch/amd64/acpi/dmar.h>

static bool apic_available = false;
static bool force_pic_mode = false;
static bool force_disable_x2apic = false;
static uint64 apic_base_phys = 0;
static volatile uint32 *apic_base_virt = NULL;
bool x2apic_enabled = false;

void apic_write(uint32 reg, uint32 val) {
    if (x2apic_enabled) {
        wrmsr(MSR_X2APIC_BASE + (reg >> 4), val);
    } else if (apic_base_virt) {
        apic_base_virt[reg / 4] = val;
        //memory barrier to ensure write is visible on real hardware
        __asm__ volatile ("mfence" ::: "memory");
    }
}

uint32 apic_read(uint32 reg) {
    if (x2apic_enabled) {
        return (uint32)rdmsr(MSR_X2APIC_BASE + (reg >> 4));
    }
    if (!apic_base_virt) return 0;
    return apic_base_virt[reg / 4];
}

bool apic_is_supported(void) {
    uint32 eax, ebx, ecx, edx;
    arch_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;
}

uint32 apic_get_id(void) {
    if (x2apic_enabled) {
        return apic_read(APIC_ID);
    }
    return (apic_read(APIC_ID) >> 24) & 0xFF;
}

void apic_set_force_pic(bool force) {
    force_pic_mode = force;
}

void apic_set_force_x2apic_disabled(bool force) {
    force_disable_x2apic = force;
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
    apic_base_phys = apic_base_msr & 0xFFFFFFFFFFFFF000ULL; //standard mask for bits 12-63

    //check if x2APIC is supported via CPUID
    uint32 eax, ebx, ecx, edx;
    arch_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if ((ecx & (1 << 21)) && !force_disable_x2apic && !dmar_x2apic_opt_out) {
        serial_write("[apic] x2APIC supported, enabling...\n");
        x2apic_enabled = true;
        //enable x2APIC (bit 10) and APIC global enable (bit 11)
        wrmsr(MSR_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE | APIC_BASE_X2APIC_ENABLE);
    } else {
        if (!(ecx & (1 << 21))) {
            serial_write("[apic] x2APIC not supported, using xAPIC (MMIO)...\n");
        } else if (force_disable_x2apic) {
            serial_write("[apic] x2APIC disabled via command line, using xAPIC (MMIO)...\n");
        } else {
            serial_write("[apic] DMAR requests x2APIC opt-out, using xAPIC (MMIO)...\n");
        }
        //verify against ACPI if available
        if (acpi_lapic_addr != 0 && acpi_lapic_addr != (uint32)apic_base_phys) {
            printf("[apic] WARNING: Physical address mismatch! MSR=0x%lx, ACPI=0x%x. Preferring ACPI.\n", 
                   apic_base_phys, acpi_lapic_addr);
            apic_base_phys = acpi_lapic_addr;
        }

        //map APIC registers using HHDM and ensure mapping exists
        apic_base_virt = (volatile uint32 *)P2V(apic_base_phys);
        vmm_kernel_map((uintptr)apic_base_virt, apic_base_phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

        //enable APIC globally via MSR
        wrmsr(MSR_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE);
    }

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
    printf("[apic] Initialized (Phys: 0x%lx, ID: %u, x2APIC: %d, Ver: 0x%x)\n", 
           apic_base_phys, apic_get_id(), x2apic_enabled, ver & 0xFF);
    serial_write("[apic] Local APIC enabled\n");
    apic_available = true;
    
    //initialize IOAPIC for legacy routing if possible
    serial_write("[apic] Initializing IOAPIC...\n");
    if (ioapic_init()) {
        //disable legacy PIC as IOAPIC now handles routing
        pic_disable();
        serial_write("[apic] Legacy PIC disabled\n");

        //attempt to enable VT-d interrupt remapping (will use x2APIC IDs)
        iommu_init();

        //if IR was enabled, re-program IOAPIC entries in remapped format
        ioapic_reconfigure_for_ir();
    } else {
        serial_write("[apic] ERR: IOAPIC initialization failed, aborting APIC setup\n");
        //if IOAPIC fails we should really stick to PIC for everything to be safe
        apic_available = false;
        return false;
    }

    return true;
}

void apic_init_ap(void) {
    if (!apic_available) return;

    //enable APIC globally for this CPU via MSR
    uint64 apic_base_msr = rdmsr(MSR_APIC_BASE);
    if (x2apic_enabled) {
        wrmsr(MSR_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE | APIC_BASE_X2APIC_ENABLE);
    } else {
        wrmsr(MSR_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE);
    }
    
    //memory barrier after enabling APIC
    __asm__ volatile ("mfence" ::: "memory");

    //set spurious interrupt vector and enable software
    //we use the same vector as the BSP
    apic_write(APIC_SPURIOUS, APIC_SPURIOUS_ENABLE | APIC_SPURIOUS_VECTOR);
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
    if (x2apic_enabled) return; //x2APIC doesn't need delivery-status polling
    while (apic_read(APIC_ICR_LOW) & (1 << 12)) arch_pause();
}

void apic_send_ipi(uint32 apic_id, uint8 vector) {
    if (x2apic_enabled) {
        //in x2APIC mode, ICR is a single 64-bit MSR
        //APIC ID goes into bits 32-63, vector+flags into bits 0-31
        uint64 icr = ((uint64)apic_id << 32) | vector | (0 << 8) | (1 << 14);
        wrmsr(MSR_X2APIC_BASE + (APIC_ICR_LOW >> 4), icr);
    } else {
        apic_wait_icr_idle();
        apic_write(APIC_ICR_HIGH, (uint32)apic_id << 24);
        apic_write(APIC_ICR_LOW, vector | (0 << 8) | (1 << 14)); //fixed, asserted
    }
}

void apic_send_init_ipi(uint32 apic_id) {
    if (x2apic_enabled) {
        //INIT: delivery mode 5 (bits 10:8), level assert (bit 14)
        uint64 icr = ((uint64)apic_id << 32) | (5 << 8) | (1 << 14);
        wrmsr(MSR_X2APIC_BASE + (APIC_ICR_LOW >> 4), icr);
    } else {
        apic_wait_icr_idle();
        apic_write(APIC_ICR_HIGH, apic_id << 24);
        apic_write(APIC_ICR_LOW, (5 << 8) | (1 << 14));
        apic_wait_icr_idle();
    }
}

void apic_send_init_deassert(uint32 apic_id) {
    if (x2apic_enabled) {
        //x2APIC: INIT de-assert is not required (and some implementations ignore it)
        return;
    }
    apic_wait_icr_idle();
    apic_write(APIC_ICR_HIGH, apic_id << 24);
    apic_write(APIC_ICR_LOW, (5 << 8) | (0 << 14)); //INIT, de-assert
}

void apic_send_startup_ipi(uint32 apic_id, uint8 vector) {
    if (x2apic_enabled) {
        //SIPI: delivery mode 6 (bits 10:8), vector = page number
        uint64 icr = ((uint64)apic_id << 32) | vector | (6 << 8);
        wrmsr(MSR_X2APIC_BASE + (APIC_ICR_LOW >> 4), icr);
    } else {
        apic_wait_icr_idle();
        apic_write(APIC_ICR_HIGH, apic_id << 24);
        apic_write(APIC_ICR_LOW, vector | (6 << 8));
        apic_wait_icr_idle();
    }
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
