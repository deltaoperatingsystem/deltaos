#include <arch/amd64/int/iommu.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/acpi/dmar.h>
#include <arch/amd64/cpu.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <lib/io.h>
#include <lib/string.h>
#include <drivers/serial.h>

bool iommu_ir_enabled = false;

static volatile uint32 *iommu_regs = NULL;
static iommu_irte_t *irt = NULL;
static uint64 irt_phys = 0;
static iommu_inv_desc_t *invq = NULL;
static uint64 invq_phys = 0;
static uint32 invq_tail = 0;
static int irt_next_free = 0;
static bool force_disable_iommu_ir = false;

void iommu_set_force_disable(bool force) {
    force_disable_iommu_ir = force;
}

static inline void iommu_write32(uint32 reg, uint32 val) {
    iommu_regs[reg / 4] = val;
    arch_mb();
}

static inline uint32 iommu_read32(uint32 reg) {
    return iommu_regs[reg / 4];
}

static inline void iommu_write64(uint32 reg, uint64 val) {
    volatile uint64 *p = (volatile uint64 *)((uintptr)iommu_regs + reg);
    *p = val;
    arch_mb();
}

static inline uint64 iommu_read64(uint32 reg) {
    volatile uint64 *p = (volatile uint64 *)((uintptr)iommu_regs + reg);
    return *p;
}

static void iommu_dump_state(const char *reason) {
    if (!iommu_regs) return;

    printf("[iommu] state dump (%s): GSTS=0x%08x FSTS=0x%08x IQH=0x%08x IQT=0x%08x IRTA=0x%lx\n",
           reason,
           iommu_read32(IOMMU_GSTS),
           iommu_read32(IOMMU_FSTS),
           iommu_read32(IOMMU_IQH),
           iommu_read32(IOMMU_IQT),
           iommu_read64(IOMMU_IRTA));
}

static int iommu_wait_gsts(uint32 bit, const char *what) {
    for (int i = 0; i < 100000; i++) {
        if (iommu_read32(IOMMU_GSTS) & bit) return 0;
        arch_pause();
    }

    serial_write("[iommu] timeout waiting for ");
    serial_write(what);
    serial_write("\n");
    iommu_dump_state(what);
    return -1;
}

static int iommu_invq_enable(void) {
    if (!(iommu_read64(IOMMU_ECAP) & ECAP_QI)) {
        serial_write("[iommu] hardware does not advertise queued invalidation\n");
        return -1;
    }

    void *invq_page_ptr = pmm_alloc(1);
    if (!invq_page_ptr) {
        serial_write("[iommu] failed to allocate invalidation queue page\n");
        return -1;
    }

    invq_phys = (uintptr)invq_page_ptr;
    invq = (iommu_inv_desc_t *)P2V(invq_phys);
    vmm_kernel_map((uintptr)invq, invq_phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);
    memset(invq, 0, PAGE_SIZE);
    invq_tail = 0;

    iommu_write64(IOMMU_IQA, invq_phys);
    iommu_write32(IOMMU_IQT, 0);
    iommu_write32(IOMMU_GCMD, GCMD_QIE);

    if (iommu_wait_gsts(GSTS_QIES, "QIES") < 0) {
        return -1;
    }

    return 0;
}

static int iommu_invq_submit(iommu_inv_desc_t desc) {
    if (!invq) return -1;

    uint32 next_tail = (invq_tail + sizeof(iommu_inv_desc_t)) & (IOMMU_INVQ_BYTES - 1);

    for (int i = 0; i < 100000; i++) {
        uint32 head = iommu_read32(IOMMU_IQH) & (IOMMU_INVQ_BYTES - 1);
        if (next_tail != head) break;
        arch_pause();
        if (i == 99999) {
            serial_write("[iommu] invalidation queue full\n");
            iommu_dump_state("invq-full");
            return -1;
        }
    }

    uint32 slot = invq_tail / sizeof(iommu_inv_desc_t);
    invq[slot] = desc;
    arch_mb();
    iommu_write32(IOMMU_IQT, next_tail);

    for (int i = 0; i < 100000; i++) {
        uint32 head = iommu_read32(IOMMU_IQH) & (IOMMU_INVQ_BYTES - 1);
        if (head == next_tail) {
            invq_tail = next_tail;
            return 0;
        }
        arch_pause();
    }

    serial_write("[iommu] timeout waiting for interrupt-cache invalidation\n");
    iommu_dump_state("iec-flush");
    return -1;
}

static int iommu_flush_iec_global(void) {
    iommu_inv_desc_t desc = {
        .lo = IOMMU_INV_DESC_IEC_GLOBAL,
        .hi = 0,
    };

    return iommu_invq_submit(desc);
}

int iommu_init(void) {
    if (force_disable_iommu_ir) {
        serial_write("[iommu] interrupt remapping disabled via command line\n");
        return -1;
    }

    if (!dmar_iommu_base || !dmar_ir_supported) {
        serial_write("[iommu] no IOMMU or IR not supported by firmware\n");
        return -1;
    }

    //map IOMMU registers
    iommu_regs = (volatile uint32 *)P2V(dmar_iommu_base);
    vmm_kernel_map((uintptr)iommu_regs, dmar_iommu_base, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    uint32 ver = iommu_read32(IOMMU_VER);
    uint64 cap  = iommu_read64(IOMMU_CAP);
    uint64 ecap = iommu_read64(IOMMU_ECAP);

    printf("[iommu] version %u.%u, CAP 0x%lx, ECAP 0x%lx\n",
           (ver >> 4) & 0xF, ver & 0xF, cap, ecap);

    //check hardware supports interrupt remapping
    if (!(ecap & ECAP_IR)) {
        serial_write("[iommu] hardware does not support interrupt remapping\n");
        return -1;
    }

    bool eim_supported = (ecap & ECAP_EIM) != 0;
    printf("[iommu] IR supported, EIM (x2APIC) %s\n",
           eim_supported ? "supported" : "not supported");

    if (iommu_invq_enable() < 0) {
        return -1;
    }

    //allocate the interrupt remap table as contiguous, page-aligned memory
    void *irt_page_ptr = pmm_alloc(IOMMU_IRT_PAGES);
    if (!irt_page_ptr) {
        serial_write("[iommu] failed to allocate IRT page\n");
        return -1;
    }
    irt_phys = (uintptr)irt_page_ptr;
    irt = (iommu_irte_t *)P2V(irt_phys);
    vmm_kernel_map((uintptr)irt, irt_phys, IOMMU_IRT_PAGES, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);
    memset(irt, 0, IOMMU_IRT_PAGES * PAGE_SIZE);

    //program IRTA register
    //bits 3:0 = size (log2(entries) - 1), bit 11 = EIME (extended interrupt mode)
    uint64 irta_val = irt_phys | (IOMMU_IRT_ORDER - 1);
    if (eim_supported && x2apic_enabled) {
        irta_val |= (1ULL << 11); //EIME: enable x2APIC destination IDs in IRTEs
    }
    iommu_write64(IOMMU_IRTA, irta_val);

    //set interrupt remap table pointer (SIRTP)
    iommu_write32(IOMMU_GCMD, GCMD_SIRTP);

    if (iommu_wait_gsts(GSTS_IRTPS, "IRTPS") < 0) {
        return -1;
    }

    if (iommu_flush_iec_global() < 0) {
        return -1;
    }

    //keep compatibility-format interrupts available so selected legacy lines
    //such as PS/2 IRQ1/IRQ12 can stay in IOAPIC compatibility mode on
    //hardware that dislikes remapped delivery for those sourced
    iommu_write32(IOMMU_GCMD, GCMD_CFI);

    if (iommu_wait_gsts(GSTS_CFIS, "CFIS") == 0) {
        serial_write("[iommu] compatibility-format interrupts enabled\n");
    } else {
        serial_write("[iommu] warning: timeout waiting for CFIS, continuing\n");
    }

    //enable interrupt remapping (IRE)
    iommu_write32(IOMMU_GCMD, GCMD_IRE);

    if (iommu_wait_gsts(GSTS_IRES, "IRES") < 0) {
        return -1;
    }

    iommu_ir_enabled = true;
    printf("[iommu] interrupt remapping enabled (IRT at 0x%lx, %d entries, EIM=%d)\n",
           irt_phys, IOMMU_IRT_SIZE, eim_supported && x2apic_enabled);

    return 0;
}

int iommu_alloc_irte(void) {
    if (irt_next_free >= IOMMU_IRT_SIZE) return -1;
    return irt_next_free++;
}

void iommu_write_irte(int index, uint32 dest_apic_id, uint8 vector,
                      uint8 delivery_mode, uint8 trigger_mode,
                      uint16 source_id, uint8 source_qualifier, uint8 source_validation) {
    if (index < 0 || index >= IOMMU_IRT_SIZE || !irt) return;

    iommu_irte_t entry;
    entry.lo = IRTE_PRESENT
             | ((uint64)(delivery_mode & 0x7) << IRTE_DLM_SHIFT)
             | ((uint64)(trigger_mode & 0x1) << IRTE_TM_SHIFT)
             | ((uint64)vector << IRTE_VECTOR_SHIFT)
             | ((uint64)dest_apic_id << IRTE_DST_SHIFT);
    entry.hi = ((uint64)source_id << IRTE_SID_SHIFT)
             | ((uint64)(source_qualifier & 0x3) << IRTE_SQ_SHIFT)
             | ((uint64)(source_validation & 0x3) << IRTE_SVT_SHIFT);

    //write atomically: clear present first, write hi, then write lo with present
    irt[index].lo = 0;
    arch_mb();
    irt[index].hi = entry.hi;
    arch_mb();
    irt[index].lo = entry.lo;
    arch_mb();

    if (iommu_ir_enabled) {
        iommu_flush_iec_global();
    }
}
