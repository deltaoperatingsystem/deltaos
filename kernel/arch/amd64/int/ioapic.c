#include <arch/amd64/int/ioapic.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/iommu.h>
#include <arch/amd64/io.h>
#include <arch/cpu.h>
#include <mm/mm.h>
#include <mm/vmm.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <drivers/serial.h>
#include <arch/amd64/acpi/acpi.h>
#include <arch/amd64/acpi/dmar.h>

static bool ioapic_available = false;
static volatile uint32 *ioapic_base = NULL;
static uint32 ioapic_max_redir = 0;
static spinlock_irq_t ioapic_lock = SPINLOCK_IRQ_INIT;
static int ioapic_gsi_irtes[256];
static bool ioapic_logged_compat_isa = false;

//low-level register access - caller MUST hold ioapic_lock
static inline uint32 ioapic_read_locked(uint8 reg) {
    if (!ioapic_base) return 0;
    ioapic_base[0] = reg;
    __asm__ volatile ("mfence" ::: "memory");
    return ioapic_base[4];
}

static inline void ioapic_write_locked(uint8 reg, uint32 val) {
    if (!ioapic_base) return;
    ioapic_base[0] = reg;
    __asm__ volatile ("mfence" ::: "memory");
    ioapic_base[4] = val;
    __asm__ volatile ("mfence" ::: "memory");
}

//public wrappers that acquire the lock
static inline uint32 ioapic_read(uint8 reg) {
    irq_state_t fl = spinlock_irq_acquire(&ioapic_lock);
    uint32 val = ioapic_read_locked(reg);
    spinlock_irq_release(&ioapic_lock, fl);
    return val;
}

static inline void ioapic_write(uint8 reg, uint32 val) {
    irq_state_t fl = spinlock_irq_acquire(&ioapic_lock);
    ioapic_write_locked(reg, val);
    spinlock_irq_release(&ioapic_lock, fl);
}

static uint32 ioapic_get_gsi(uint8 irq) {
    for (uint32 i = 0; i < acpi_iso_count; i++) {
        if (acpi_isos[i].irq_source == irq) {
            return acpi_isos[i].gsi;
        }
    }
    return irq;
}

static uint16 ioapic_get_irq_flags(uint8 irq) {
    for (uint32 i = 0; i < acpi_iso_count; i++) {
        if (acpi_isos[i].irq_source == irq) {
            return acpi_isos[i].flags;
        }
    }
    return 0;
}

static uint32 ioapic_build_rte_low(uint8 irq, uint8 vector, bool masked, bool remapped,
                                   uint8 *trigger_mode_out) {
    uint16 acpi_flags = ioapic_get_irq_flags(irq);
    bool active_low = false;
    bool level_triggered = false;

    switch (acpi_flags & 0x3) {
        case 0x3:
            active_low = true;
            break;
        case 0x0:
        case 0x1:
        default:
            active_low = false;
            break;
    }

    switch ((acpi_flags >> 2) & 0x3) {
        case 0x3:
            level_triggered = true;
            break;
        case 0x0:
        case 0x1:
        default:
            level_triggered = false;
            break;
    }

    if (trigger_mode_out) {
        *trigger_mode_out = level_triggered ? 1 : 0;
    }

    if (remapped) {
        uint32 low = vector;
        if (active_low) {
            low |= IOAPIC_INTPOL_LOW;
        }
        if (level_triggered) {
            low |= IOAPIC_TRIGGER_LEVEL;
        }
        if (masked) {
            low |= IOAPIC_INT_MASKED;
        }
        return low;
    }

    uint32 low = vector | IOAPIC_DELMOD_FIXED;
    if (active_low) {
        low |= IOAPIC_INTPOL_LOW;
    }
    if (level_triggered) {
        low |= IOAPIC_TRIGGER_LEVEL;
    }
    if (masked) {
        low |= IOAPIC_INT_MASKED;
    }
    return low;
}

static bool ioapic_use_compat_format(uint8 irq) {
    return irq < 16;
}

bool ioapic_init(void) {
    serial_write("[ioapic] Mapping registers...\n");
    uint32 bsp_apic_id = apic_get_id();
    uintptr phys = acpi_ioapic_addr != 0 ? acpi_ioapic_addr : IOAPIC_DEFAULT_BASE;
    ioapic_base = (volatile uint32 *)P2V(phys);
    memset(ioapic_gsi_irtes, 0xFF, sizeof(ioapic_gsi_irtes));
    
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
        //default mapping (GSI = index)
        ioapic_set_irq(i, 32 + i, bsp_apic_id, true);
    }
    
    //remap specifically for overrides found in ACPI
    for (uint32 i = 0; i < acpi_iso_count; i++) {
        if (acpi_isos[i].gsi < ioapic_max_redir) {
            printf("[ioapic] Applying ISO: IRQ %u -> GSI %u (flags 0x%x)\n", 
                   acpi_isos[i].irq_source, acpi_isos[i].gsi, acpi_isos[i].flags);
            //we only set the mapping, usually IRQ 0 -> GSI 2
            ioapic_set_irq(acpi_isos[i].irq_source, 32 + acpi_isos[i].irq_source, bsp_apic_id, true);
        }
    }
    
    ioapic_available = true;
    return true;
}

void ioapic_set_irq(uint8 irq, uint8 vector, uint32 dest_apic_id, bool masked) {
    if (irq >= ioapic_max_redir) return;

    uint32 gsi = ioapic_get_gsi(irq);
    if (gsi >= ioapic_max_redir) return;
    uint32 local_pin = (gsi >= acpi_ioapic_gsi_base) ? (gsi - acpi_ioapic_gsi_base) : gsi;

    uint32 low, high;

    if (iommu_ir_enabled && !ioapic_use_compat_format(irq)) {
        int irte = ioapic_gsi_irtes[gsi];
        if (irte < 0) {
            //allocate one IRTE per routed GSI and reuse it on later updates
            irte = iommu_alloc_irte();
            if (irte < 0) {
                printf("[ioapic] ERR: out of IRTEs for IRQ %u (GSI %u)\n", irq, gsi);
                return;
            }
            ioapic_gsi_irtes[gsi] = irte;
        }

        uint8 trigger_mode = 0;
        low = ioapic_build_rte_low(irq, (uint8)local_pin, masked, true, &trigger_mode);

        uint16 source_id = 0;
        uint8 source_qualifier = IRTE_SQ_ALL_16;
        uint8 source_validation = IRTE_SVT_NONE;
        if (dmar_ioapic_sid_valid) {
            source_id = dmar_ioapic_sid;
            source_validation = IRTE_SVT_VERIFY_SID_SQ;
        }

        //program the IRTE with the actual interrupt semantics from ACPI/MADT
        iommu_write_irte(irte, dest_apic_id, vector, 0 /*fixed*/, trigger_mode,
                         source_id, source_qualifier, source_validation);

        //remapped IOAPIC RTE layout:
        //low[7:0] = local IOAPIC pin (subhandle), low[11] = irte[15],
        //high[16] = format bit, high[31:17] = irte[14:0]
        if ((uint32)irte & (1U << 15)) {
            low |= (1U << 11);
        }
        high = (1U << 16) | (((uint32)irte & 0x7FFF) << 17);
    } else {
        if (iommu_ir_enabled && irq < 16 && !ioapic_logged_compat_isa) {
            serial_write("[ioapic] keeping legacy ISA IRQs in compatibility format under VT-d\n");
            ioapic_logged_compat_isa = true;
        }
        low = ioapic_build_rte_low(irq, vector, masked, false, NULL);
        high = (dest_apic_id & 0xFF) << 24;
    }

    irq_state_t fl = spinlock_irq_acquire(&ioapic_lock);
    ioapic_write_locked(IOAPIC_REDTBL_BASE + gsi * 2, low);
    ioapic_write_locked(IOAPIC_REDTBL_BASE + gsi * 2 + 1, high);
    spinlock_irq_release(&ioapic_lock, fl);
}

void ioapic_mask_irq(uint8 irq) {
    uint32 gsi = ioapic_get_gsi(irq);
    if (gsi >= ioapic_max_redir) return;
    irq_state_t fl = spinlock_irq_acquire(&ioapic_lock);
    uint32 low = ioapic_read_locked(IOAPIC_REDTBL_BASE + gsi * 2);
    ioapic_write_locked(IOAPIC_REDTBL_BASE + gsi * 2, low | IOAPIC_INT_MASKED);
    spinlock_irq_release(&ioapic_lock, fl);
}

void ioapic_unmask_irq(uint8 irq) {
    uint32 gsi = ioapic_get_gsi(irq);
    if (gsi >= ioapic_max_redir) return;
    irq_state_t fl = spinlock_irq_acquire(&ioapic_lock);
    uint32 low = ioapic_read_locked(IOAPIC_REDTBL_BASE + gsi * 2);
    ioapic_write_locked(IOAPIC_REDTBL_BASE + gsi * 2, low & ~IOAPIC_INT_MASKED);
    spinlock_irq_release(&ioapic_lock, fl);
}

bool ioapic_is_enabled(void) {
    return ioapic_available;
}

void ioapic_reconfigure_for_ir(void) {
    if (!ioapic_available || !iommu_ir_enabled) return;

    uint32 bsp_apic_id = apic_get_id();
    serial_write("[ioapic] re-configuring redirection table for interrupt remapping\n");

    //re-program all entries in remapped format
    for (uint8 i = 0; i < ioapic_max_redir; i++) {
        ioapic_set_irq(i, 32 + i, bsp_apic_id, true);
    }

    //re-apply ACPI interrupt source overrides
    for (uint32 i = 0; i < acpi_iso_count; i++) {
        if (acpi_isos[i].gsi < ioapic_max_redir) {
            ioapic_set_irq(acpi_isos[i].irq_source, 32 + acpi_isos[i].irq_source, bsp_apic_id, true);
        }
    }

    serial_write("[ioapic] redirection table updated for remapped format\n");
}
