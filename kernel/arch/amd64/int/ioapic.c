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
static uint32 ioapic_max_redir = 0;

static inline uint32 ioapic_read(uint8 reg) {
    if (!ioapic_base) return 0;
    ioapic_base[0] = reg;
    __asm__ volatile ("mfence" ::: "memory");
    return ioapic_base[4];
}

static inline void ioapic_write(uint8 reg, uint32 val) {
    if (!ioapic_base) return;
    ioapic_base[0] = reg;
    __asm__ volatile ("mfence" ::: "memory");
    ioapic_base[4] = val;
    __asm__ volatile ("mfence" ::: "memory");
}

bool ioapic_init(void) {
    serial_write("[ioapic] Mapping registers...\n");
    uintptr phys = IOAPIC_DEFAULT_BASE;
    ioapic_base = (volatile uint32 *)P2V(phys);
    
    vmm_kernel_map((uintptr)ioapic_base, phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);
    
    //memory barrier after mapping
    __asm__ volatile ("mfence" ::: "memory");
    
    //try to read version register - if this fails IOAPIC might not be at this address
    uint32 ver = ioapic_read(IOAPIC_VER);
    
    //check if version register looks valid (should be non-zero and have reasonable values)
    //version register format: bits 0-7 = version, bits 16-23 = max redirection entries
    if (ver == 0 || ver == 0xFFFFFFFF) {
        serial_write("[ioapic] ERR: Invalid version register (0x");
        char hex_buf[16];
        snprintf(hex_buf, sizeof(hex_buf), "%08x", ver);
        serial_write(hex_buf);
        serial_write("), IOAPIC may not be present at 0xFEC00000\n");
        ioapic_base = NULL;
        return false;
    }
    
    ioapic_max_redir = ((ver >> 16) & 0xFF) + 1;
    
    //sanity check: max redirection entries should be reasonable (typically 16-24, but up to 240 on some intel)
    if (ioapic_max_redir == 0 || ioapic_max_redir > 255) {
        serial_write("[ioapic] ERR: Invalid max redirection entries: ");
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", ioapic_max_redir);
        serial_write(buf);
        serial_write("\n");
        ioapic_base = NULL;
        return false;
    }
    
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
    (void)dest_apic_id;
    if (irq >= ioapic_max_redir) return;
    
    uint32 low = vector | IOAPIC_DELMOD_FIXED;
    if (masked) low |= IOAPIC_INT_MASKED;
    
    uint32 high = (uint32)apic_get_id() << 24;
    
    // printf("[ioapic] mapping GSI %u -> vector %u (dest %u)\n", irq, vector, apic_get_id());
    
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
