#ifndef ARCH_AMD64_IRQ_H
#define ARCH_AMD64_IRQ_H

#include <arch/amd64/types.h>

/*
 * architecture-independent MSI/MSI-X composition interface
 *
 * drivers call irq_compose_msi() to get the platform-specific
 * address/data values needed for MSI or MSI-X table entries
 * the arch layer handles APIC routing, IOMMU remapping, etc
 */

typedef struct {
    uint32 addr_lo;
    uint32 addr_hi;
    uint32 data;
} irq_msi_msg_t;

//compose MSI address/data for a given vector, targeting the BSP
//handles IOMMU interrupt remapping transparently if available
int irq_compose_msi(uint8 vector, irq_msi_msg_t *msg);

#endif
