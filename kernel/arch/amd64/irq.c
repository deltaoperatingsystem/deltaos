#include <arch/irq.h>
#include <arch/amd64/int/apic.h>
#include <arch/amd64/int/iommu.h>
#include <arch/amd64/percpu.h>
#include <lib/io.h>

int irq_compose_msi(uint8 vector, irq_msi_msg_t *msg) {
    //target the BSP
    uint32 dest = apic_get_id();
    percpu_t *bsp = percpu_get_by_index(0);
    if (bsp) dest = bsp->apic_id;

    if (iommu_ir_enabled) {
        //remapped format: allocate an IRTE and encode its index
        int irte = iommu_alloc_irte();
        if (irte < 0) return -1;
        iommu_write_irte(irte, dest, vector, 0 /*fixed*/, 0 /*edge*/,
                         0, IRTE_SQ_ALL_16, IRTE_SVT_NONE);

        //VT-d remapped MSI address format:
        // bits [19:5] = interrupt_index[14:0]
        // bit  [4]    = interrupt_format (1 = remapped)
        // bit  [2]    = interrupt_index[15]
        msg->addr_lo = 0xFEE00000
            | (((uint32)irte & 0x7FFF) << 5)
            | (1 << 4)
            | (((uint32)irte >> 15) << 2);
        msg->addr_hi = 0;
        msg->data    = 0; //subhandle
    } else {
        //legacy xAPIC format
        msg->addr_lo = 0xFEE00000 | ((uint32)(dest & 0xFF) << 12);
        msg->addr_hi = 0;
        msg->data    = vector;
    }

    return 0;
}
