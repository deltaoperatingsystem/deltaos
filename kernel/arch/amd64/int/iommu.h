#ifndef ARCH_AMD64_INT_IOMMU_H
#define ARCH_AMD64_INT_IOMMU_H

#include <arch/types.h>

//IOMMU register offsets (from intel VT-d spec)
#define IOMMU_VER           0x000  //version
#define IOMMU_CAP           0x008  //capability
#define IOMMU_ECAP          0x010  //extended capability
#define IOMMU_GCMD          0x018  //global command
#define IOMMU_GSTS          0x01C  //global status
#define IOMMU_FSTS          0x034  //fault status
#define IOMMU_IQH           0x080  //invalidation queue head
#define IOMMU_IQT           0x088  //invalidation queue tail
#define IOMMU_IQA           0x090  //invalidation queue address
#define IOMMU_IRTA          0x0B8  //interrupt remapping table address

//GCMD bits
#define GCMD_SIRTP          (1U << 24)  //set interrupt remap table pointer
#define GCMD_IRE            (1U << 25)  //interrupt remapping enable
#define GCMD_QIE            (1U << 26)  //queued invalidation enable
#define GCMD_CFI            (1U << 23)  //compatibility format interrupts

//GSTS bits
#define GSTS_IRTPS          (1U << 24)  //interrupt remap table pointer status
#define GSTS_IRES           (1U << 25)  //interrupt remapping enabled status
#define GSTS_QIES           (1U << 26)  //queued invalidation enabled status
#define GSTS_CFIS           (1U << 23)  //compatibility format interrupt status

//ECAP bits
#define ECAP_QI             (1ULL << 1) //queued invalidation support
#define ECAP_IR             (1ULL << 3) //interrupt remapping support
#define ECAP_EIM            (1ULL << 4) //extended interrupt mode (x2APIC)

//IRTE (interrupt remapping table entry) - 128 bits
typedef struct iommu_irte {
    uint64 lo;
    uint64 hi;
} __attribute__((packed)) iommu_irte_t;

//IRTE low word fields
#define IRTE_PRESENT        (1ULL << 0)
#define IRTE_DLM_SHIFT      5           //delivery mode (3 bits)
#define IRTE_TM_SHIFT       4           //trigger mode (0=edge, 1=level)
#define IRTE_VECTOR_SHIFT   16
#define IRTE_DST_SHIFT      32          //destination APIC ID

//IRTE high word fields
#define IRTE_SID_SHIFT      0           //source-id (16 bits)
#define IRTE_SQ_SHIFT       16          //source-id qualifier (2 bits)
#define IRTE_SVT_SHIFT      18          //source validation type (2 bits)

#define IRTE_SQ_ALL_16      0
#define IRTE_SVT_NONE       0
#define IRTE_SVT_VERIFY_SID_SQ 1

typedef struct iommu_inv_desc {
    uint64 lo;
    uint64 hi;
} __attribute__((packed)) iommu_inv_desc_t;

#define IOMMU_INVQ_ORDER    8
#define IOMMU_INVQ_SIZE     (1U << IOMMU_INVQ_ORDER)
#define IOMMU_INVQ_BYTES    (IOMMU_INVQ_SIZE * sizeof(iommu_inv_desc_t))

#define IOMMU_INV_DESC_IEC_GLOBAL 0x4


#define IOMMU_IRT_SIZE      1024
#define IOMMU_IRT_ORDER     10          //log2(1024) for IRTA size encoding
#define IOMMU_IRT_PAGES     4

//interface
extern bool iommu_ir_enabled;

void iommu_set_force_disable(bool force);
int  iommu_init(void);
int  iommu_alloc_irte(void);
void iommu_write_irte(int index, uint32 dest_apic_id, uint8 vector,
                      uint8 delivery_mode, uint8 trigger_mode,
                      uint16 source_id, uint8 source_qualifier, uint8 source_validation);

#endif
