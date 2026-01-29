#ifndef DRIVERS_NVME_H
#define DRIVERS_NVME_H

#include <arch/types.h>
#include <drivers/pci.h>
#include <proc/wait.h>
#include <lib/spinlock.h>

//NVMe register offsets (MMIO)
#define NVME_REG_CAP     0x00   //controller capabilities
#define NVME_REG_VS      0x08   //version
#define NVME_REG_INTMS   0x0C   //interrupt mask set
#define NVME_REG_INTMC   0x10   //interrupt mask clear
#define NVME_REG_CC      0x14   //controller configuration
#define NVME_REG_CSTS    0x1C   //controller status
#define NVME_REG_NSSR    0x20   //NVM subsystem reset
#define NVME_REG_AQA     0x24   //admin queue attributes
#define NVME_REG_ASQ     0x28   //admin submission queue base address
#define NVME_REG_ACQ     0x30   //admin completion queue base address

//doorbell stride is calculated from CAP.DSTRD (1 << (2 + cap.dstrd))
#define NVME_REG_DBL(q, is_cq, dstrd) (0x1000 + ((q) * 2 + (is_cq ? 1 : 0)) * (4 << (dstrd)))

//controller configuration bits
#define NVME_CC_EN       (1 << 0)
#define NVME_CC_CSS_NVM  (0 << 4)
#define NVME_CC_MPS_4K   (0 << 7)
#define NVME_CC_AMS_RR   (0 << 11)
#define NVME_CC_SHN_NONE (0 << 14)
#define NVME_CC_IOSQES   (6 << 16) //2^6 = 64 bytes
#define NVME_CC_IOCQES   (4 << 20) //2^4 = 16 bytes

//controller status bits
#define NVME_CSTS_RDY    (1 << 0)
#define NVME_CSTS_CFS    (1 << 1)

//NVMe opcodes (admin)
#define NVME_OP_DELETE_I_SQ    0x00
#define NVME_OP_CREATE_I_SQ    0x01
#define NVME_OP_GET_LOG_PAGE   0x02
#define NVME_OP_DELETE_I_CQ    0x04
#define NVME_OP_CREATE_I_CQ    0x05
#define NVME_OP_IDENTIFY       0x06
#define NVME_OP_SET_FEATURES   0x09

//NVMe opcodes (NVM)
#define NVME_OP_WRITE          0x01
#define NVME_OP_READ           0x02

//64-byte submission queue entry (SQE)
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

//16-byte completion queue wntry (CQE)
typedef struct {
    uint32 command_specific;
    uint32 reserved;
    uint16 sq_head;
    uint16 sq_id;
    uint16 command_id;
    uint16 status;
} __attribute__((packed)) nvme_cqe_t;

//identify structures 
typedef struct {
    uint16 vid;         //0
    uint16 ssvid;       //2
    char   sn[20];      //4
    char   mn[40];      //24
    char   fr[8];       //64
    uint8  rab;         //72
    uint8  ieee[3];     //73
    uint8  mic;         //76
    uint8  mdts;        //77
    uint16 ctrl_id;     //78
    uint32 ver;         //80
    uint32 rtd3r;       //84
    uint32 rtd3e;       //88
    uint32 oaes;        //92
    uint32 ctratt;      //96
    uint16 rrls;        //100
    uint8  reserved0[26]; //102
    uint8  tnvmcap[16];   //128
    uint8  unvmcap[16];   //144
    uint8  reserved1[352]; //160
    uint8  sqes;       //512
    uint8  cqes;       //513
    uint16 maxcmd;     //514
    uint32 nn;         //516
    uint8  reserved3[4096 - 520];
} __attribute__((packed)) nvme_identify_ctrl_t;

typedef struct {
    uint64 ns_size;    //0
    uint64 ns_cap;     //8
    uint64 ns_use;     //16
    uint8  features;   //24
    uint8  nlbaf;      //25
    uint8  flbas;      //26
    uint8  mc;         //27
    uint8  dpc;        //28
    uint8  dps;        //29
    uint8  nmic;       //30
    uint8  rescap;     //31
    uint8  fpi;        //32
    uint8  dlfeat;     //33
    uint16 awun;       //34
    uint16 awupf;      //36
    uint16 acwu;       //38
    uint8  reserved1[88]; //40
    uint32 lbaf[16];   //128
    uint8  reserved3[4096 - 192];
} __attribute__((packed)) nvme_identify_ns_t;

//MSI-X structures 
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
    spinlock_t  lock;
} nvme_queue_t;

typedef struct nvme_ctrl nvme_ctrl_t;

typedef struct {
    nvme_ctrl_t *ctrl;
    uint32      nsid;
    uint64      sector_count;
    uint32      sector_size;
    void        *obj; // object_t*
} nvme_ns_t;

struct nvme_ctrl {
    pci_device_t *pci;
    void        *regs;
    size        dstrd;
    
    //admin queue 
    nvme_queue_t admin_q;
    
    //I/O queues 
    nvme_queue_t io_q[NVME_MAX_IO_QUEUES];
    uint16      num_io_queues;
    
    size        max_transfer_shift;
    
    //Namespaces
    nvme_ns_t   *ns;
    uint32      num_ns;
    
    //MSI-X 
    uint16      msix_cap_ptr;
    msix_table_entry_t *msix_table;
    uint64      int_count;
    uint32      ctrl_idx;
};

void nvme_init(void);
void nvme_msix_handler(nvme_ctrl_t *ctrl, uint16 qid);
void nvme_isr_callback(uint64 vector);

#endif
