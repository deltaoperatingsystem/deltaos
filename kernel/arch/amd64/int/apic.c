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
static uint64 apic_base_phys = 0;
static volatile uint32 *apic_base_virt = NULL;

void apic_write(uint32 reg, uint32 val) {
    if (apic_base_virt) apic_base_virt[reg / 4] = val;
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

bool apic_init(void) {
    serial_write("[apic] Initializing...\n");
    if (!apic_is_supported()) {
        serial_write("[apic] ERR: APIC not supported\n");
        return false;
    }

    uint64 apic_base_msr = rdmsr(MSR_APIC_BASE);
    apic_base_phys = apic_base_msr & 0x7FFFFFFFFFFF000ULL;

    //map APIC registers using HHDM and ensure mapping exists
    apic_base_virt = (volatile uint32 *)P2V(apic_base_phys);
    vmm_kernel_map((uintptr)apic_base_virt, apic_base_phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    //enable APIC globally via MSR
    wrmsr(MSR_APIC_BASE, apic_base_msr | APIC_BASE_ENABLE);

    //set spurious interrupt vector and enable software
    apic_write(APIC_SPURIOUS, APIC_SPURIOUS_ENABLE | APIC_SPURIOUS_VECTOR);

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
