#ifndef ARCH_AMD64_ACPI_DMAR_H
#define ARCH_AMD64_ACPI_DMAR_H

#include <arch/amd64/acpi/acpi.h>

#define ACPI_DMAR_SIGNATURE "DMAR"

//DMAR table header (follows the standard ACPI header)
typedef struct acpi_dmar {
    acpi_header_t header;
    uint8 host_address_width;  //maximum DMA physical address width - 1
    uint8 flags;               //bit 0 = INTR_REMAP, bit 1 = X2APIC_OPT_OUT
    uint8 reserved[10];
    //remapping structures follow
} __attribute__((packed)) acpi_dmar_t;

#define DMAR_FLAG_INTR_REMAP    (1 << 0)
#define DMAR_FLAG_X2APIC_OPT_OUT (1 << 1)

//remapping structure types
#define DMAR_TYPE_DRHD  0  //DMA remapping hardware unit definition
#define DMAR_TYPE_RMRR  1  //reserved nemory region reporting
#define DMAR_TYPE_ATSR  2  //root port ATS capability reporting
#define DMAR_TYPE_RHSA  3  //remapping hardware static affinity
#define DMAR_TYPE_ANDD  4  //ACPI name-space device declaration

//common header for all remapping structures
typedef struct dmar_remap_header {
    uint16 type;
    uint16 length;
} __attribute__((packed)) dmar_remap_header_t;

//DRHD - DMA remapping hardware unit definition
typedef struct dmar_drhd {
    dmar_remap_header_t header;
    uint8 flags;       //bit 0 = INCLUDE_PCI_ALL
    uint8 reserved;
    uint16 segment;    //PCI segment number
    uint64 register_base_address;
    //device scope entries follow
} __attribute__((packed)) dmar_drhd_t;

#define DRHD_FLAG_INCLUDE_PCI_ALL (1 << 0)

//device scope types
#define DMAR_SCOPE_ENDPOINT            0x01
#define DMAR_SCOPE_BRIDGE              0x02
#define DMAR_SCOPE_IOAPIC              0x03
#define DMAR_SCOPE_HPET                0x04
#define DMAR_SCOPE_NAMESPACE_DEVICE    0x05

typedef struct dmar_dev_scope {
    uint8 type;
    uint8 length;
    uint16 reserved;
    uint8 enumeration_id;
    uint8 start_bus;
    //PCI path entries follow
} __attribute__((packed)) dmar_dev_scope_t;

typedef struct dmar_pci_path {
    uint8 device;
    uint8 function;
} __attribute__((packed)) dmar_pci_path_t;

//parsed DMAR info exported for the IOMMU driver
extern uint64 dmar_iommu_base;      //register base of the INCLUDE_PCI_ALL DRHD
extern bool   dmar_ir_supported;    //firmware advertises interrupt remapping
extern bool   dmar_x2apic_opt_out;  //firmware requests x2APIC stay disabled
extern uint8  dmar_host_aw;         //host address width
extern bool   dmar_ioapic_sid_valid;
extern uint16 dmar_ioapic_sid;

void dmar_init(void);

#endif
