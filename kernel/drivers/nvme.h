#ifndef DRIVERS_NVME_H
#define DRIVERS_NVME_H

#include <arch/types.h>
#include <drivers/pci.h>
#include <proc/wait.h>

/* NVMe Register Offsets (MMIO) */
#define NVME_REG_CAP     0x00   /* Controller Capabilities */
#define NVME_REG_VS      0x08   /* Version */
#define NVME_REG_INTMS   0x0C   /* Interrupt Mask Set */
#define NVME_REG_INTMC   0x10   /* Interrupt Mask Clear */
#define NVME_REG_CC      0x14   /* Controller Configuration */
#define NVME_REG_CSTS    0x1C   /* Controller Status */
#define NVME_REG_NSSR    0x20   /* NVM Subsystem Reset */
#define NVME_REG_AQA     0x24   /* Admin Queue Attributes */
#define NVME_REG_ASQ     0x28   /* Admin Submission Queue Base Address */
#define NVME_REG_ACQ     0x30   /* Admin Completion Queue Base Address */

/* Doorbell stride is calculated from CAP.DSTRD (1 << (2 + cap.dstrd)) */
#define NVME_REG_DBL(q, is_cq, dstrd) (0x1000 + ((q) * 2 + (is_cq ? 1 : 0)) * (4 << (dstrd)))

/* Controller Configuration Bits */
#define NVME_CC_EN       (1 << 0)
#define NVME_CC_CSS_NVM  (0 << 4)
#define NVME_CC_MPS_4K   (0 << 7)
#define NVME_CC_AMS_RR   (0 << 11)
#define NVME_CC_SHN_NONE (0 << 14)
#define NVME_CC_IOSQES   (6 << 16) /* 2^6 = 64 bytes */
#define NVME_CC_IOCQES   (4 << 20) /* 2^4 = 16 bytes */

/* Controller Status Bits */
#define NVME_CSTS_RDY    (1 << 0)
#define NVME_CSTS_CFS    (1 << 1)

/* NVMe Opcodes (Admin) */
#define NVME_OP_DELETE_I_SQ    0x00
#define NVME_OP_CREATE_I_SQ    0x01
#define NVME_OP_GET_LOG_PAGE   0x02
#define NVME_OP_DELETE_I_CQ    0x04
#define NVME_OP_CREATE_I_CQ    0x05
#define NVME_OP_IDENTIFY       0x06
#define NVME_OP_SET_FEATURES   0x09

/* NVMe Opcodes (NVM) */
#define NVME_OP_WRITE          0x01
#define NVME_OP_READ           0x02

/* 64-byte Submission Queue Entry (SQE) */
typedef struct {
    uint8  opcode;
    uint8  flags;
    uint16 command_id;
    uint32 nsid;
    uint32 reserved[2];
    uint64 metadata;
    uint64 prp1;
    uint64 prp2;
    uint32 cdw10;
    uint32 cdw11;
    uint32 cdw12;
    uint32 cdw13;
    uint32 cdw14;
    uint32 cdw15;
} __attribute__((packed)) nvme_sqe_t;

/* 16-byte Completion Queue Entry (CQE) */
typedef struct {
    uint32 command_specific;
    uint32 reserved;
    uint16 sq_head;
    uint16 sq_id;
    uint16 command_id;
    uint16 status;
} __attribute__((packed)) nvme_cqe_t;

/* Identify Structures */
typedef struct {
    uint16 vid;
    uint16 ssvid;
    char   sn[20];
    char   mn[40];
    char   fr[8];
    /* ... lots of other fields, we just need basic ones for now ... */
    uint8  reserved[4096 - 72];
} __attribute__((packed)) nvme_identify_ctrl_t;

typedef struct {
    uint64 ns_size;    /* total size in logical blocks */
    uint64 ns_cap;
    uint64 ns_use;
    uint8  features;
    uint8  nlbaf;      /* number of LBA formats */
    uint8  flbas;      /* formatted LBA size */
    uint8  mc;
    uint8  dpc;
    uint8  dps;
    uint8  nmic;
    uint8  rescap;
    uint8  fpi;
    uint8  dlfeat;
    uint8  awun;
    uint8  awupf;
    uint8  acwu;
    uint8  reserved1[2];
    uint32 nzapa;
    uint8  reserved2[85];
    /* ... LBA formats start at byte 128 ... */
    uint32 lbaf[16];
    uint8  reserved3[4096 - 192];
} __attribute__((packed)) nvme_identify_ns_t;

/* PCI MSI-X Structures */
typedef struct {
    uint32 msg_addr_low;
    uint32 msg_addr_high;
    uint32 msg_data;
    uint32 vector_control;
} __attribute__((packed)) msix_table_entry_t;

#define PCI_CAP_ID_MSIX 0x11

#define NVME_MAX_IO_QUEUES 8

typedef struct {
    nvme_sqe_t  *sq;
    nvme_cqe_t  *cq;
    uint16      sq_tail;
    uint16      cq_head;
    uint16      cq_phase;
    wait_queue_t wq;
    uint32      db_sq;
    uint32      db_cq;
} nvme_queue_t;

typedef struct {
    pci_device_t *pci;
    void        *regs;
    size        dstrd;
    
    /* Admin Queue */
    nvme_queue_t admin_q;
    
    /* I/O Queues */
    nvme_queue_t io_q[NVME_MAX_IO_QUEUES];
    uint16      num_io_queues;
    
    size        max_transfer_shift;
    uint32      nsid;
    uint64      sector_count;
    uint32      sector_size;
    
    /* MSI-X */
    uint16      msix_cap_ptr;
    msix_table_entry_t *msix_table;
    uint64      int_count;
} nvme_ctrl_t;

void nvme_init(void);
void nvme_msix_handler(nvme_ctrl_t *ctrl, uint16 qid);
void nvme_isr_callback(uint64 vector);

#endif
