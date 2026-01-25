#include <arch/amd64/int/ioapic.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/io.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <lib/io.h>
#include <lib/string.h>
#include <drivers/serial.h>

static bool ioapic_available = false;
static volatile uint32 *ioapic_base = NULL;
static uint8 ioapic_max_redir = 0;

static inline uint32 ioapic_read(uint8 reg) {
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

static inline void ioapic_write(uint8 reg, uint32 val) {
    ioapic_base[0] = reg;
    ioapic_base[4] = val;
}

bool ioapic_init(void) {
    serial_write("[ioapic] Mapping registers...\n");
    uintptr phys = IOAPIC_DEFAULT_BASE;
    ioapic_base = (volatile uint32 *)P2V(phys);
    
    vmm_kernel_map((uintptr)ioapic_base, phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);
    
    uint32 ver = ioapic_read(IOAPIC_VER);
    ioapic_max_redir = ((ver >> 16) & 0xFF) + 1;
    
    printf("[ioapic] Initialized (Phys: 0x%lx, Max IRQs: %u)\n", phys, ioapic_max_redir);
    serial_write("[ioapic] Redirection table configured\n");
    
    //mask everything initially
    for (uint8 i = 0; i < ioapic_max_redir; i++) {
        ioapic_set_irq(i, 32 + i, 0, true);
    }
    
    ioapic_available = true;
    return true;
}

void ioapic_set_irq(uint8 irq, uint8 vector, uint8 dest_apic_id, bool masked) {
    if (irq >= ioapic_max_redir) return;
    
    uint32 low = vector | IOAPIC_DELMOD_FIXED;
    if (masked) low |= IOAPIC_INT_MASKED;
    
    uint32 high = (uint32)dest_apic_id << 24;
    
    ioapic_write(IOAPIC_REDTBL_BASE + irq * 2, low);
    ioapic_write(IOAPIC_REDTBL_BASE + irq * 2 + 1, high);
}

void ioapic_mask_irq(uint8 irq) {
    if (irq >= ioapic_max_redir) return;
    uint32 low = ioapic_read(IOAPIC_REDTBL_BASE + irq * 2);
    ioapic_write(IOAPIC_REDTBL_BASE + irq * 2, low | IOAPIC_INT_MASKED);
}

void ioapic_unmask_irq(uint8 irq) {
    if (irq >= ioapic_max_redir) return;
    uint32 low = ioapic_read(IOAPIC_REDTBL_BASE + irq * 2);
    ioapic_write(IOAPIC_REDTBL_BASE + irq * 2, low & ~IOAPIC_INT_MASKED);
}

bool ioapic_is_enabled(void) {
    return ioapic_available;
}
