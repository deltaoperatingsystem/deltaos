#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <arch/mmu.h>
#include <lib/io.h>
#include <lib/string.h>
#include <proc/sched.h>
#include <proc/thread.h>
#include <drivers/init.h>
#include <arch/cpu.h>
#include <obj/namespace.h>
#include <obj/object.h>
#include <drivers/blkdev.h>
#include <drivers/gpt.h>
#include <fs/fs.h>

#define NVME_QUEUE_SIZE 64

static nvme_ctrl_t *ctrls[4];
static uint32 ctrl_count = 0;

static ssize nvme_read_op(object_t *obj, void *buf, size len, size offset);
static ssize nvme_write_op(object_t *obj, const void *buf, size len, size offset);
static int nvme_stat(object_t *obj, stat_t *st);
static int nvme_discover_namespaces(nvme_ctrl_t *ctrl);


static object_ops_t nvme_ops = {
    .read = nvme_read_op,
    .write = nvme_write_op,
    .close = NULL, 
    .readdir = NULL,
    .lookup = NULL,
    .stat = nvme_stat
};

static int nvme_blkdev_read(blkdev_t *dev, uint64 lba, uint32 count, void *buf);
static int nvme_blkdev_write(blkdev_t *dev, uint64 lba, uint32 count, const void *buf);

static blkdev_ops_t nvme_blkdev_ops = {
    .read = nvme_blkdev_read,
    .write = nvme_blkdev_write,
};

static void nvme_trim(char *str, size len) {
    for (int i = len - 1; i >= 0; i--) {
        if (str[i] == ' ' || str[i] == '\0') str[i] = '\0';
        else break;
    }
}

void nvme_msix_handler(nvme_ctrl_t *ctrl, uint16 qid) {
    ctrl->int_count++;
    if (qid == 0) {
        thread_wake_one(&ctrl->admin_q.wq);
    } else if (qid <= ctrl->num_io_queues) {
        thread_wake_one(&ctrl->io_q[qid - 1].wq);
    }
}

void nvme_isr_callback(uint64 vector) {
    if (vector < 0x40 || vector > 0x47) return;
    uint16 qid = vector - 0x40;
    
    for (uint32 i = 0; i < ctrl_count; i++) {
        if (ctrls[i]) nvme_msix_handler(ctrls[i], qid);
    }
}

static void nvme_write32(nvme_ctrl_t *ctrl, uint32 reg, uint32 val) {
    *(volatile uint32 *)((uintptr)ctrl->regs + reg) = val;
}

static uint32 nvme_read32(nvme_ctrl_t *ctrl, uint32 reg) {
    return *(volatile uint32 *)((uintptr)ctrl->regs + reg);
}

static void nvme_write64(nvme_ctrl_t *ctrl, uint32 reg, uint64 val) {
    *(volatile uint64 *)((uintptr)ctrl->regs + reg) = val;
}

static uint64 nvme_read64(nvme_ctrl_t *ctrl, uint32 reg) {
    return *(volatile uint64 *)((uintptr)ctrl->regs + reg);
}

//wait for completion on a specific queue
static int nvme_wait_completion(nvme_ctrl_t *ctrl, nvme_queue_t *q, uint16 cmd_id, uint32 *cdw0) {
    thread_t *current = thread_current();
    bool use_interrupts = (current != NULL && current->state == THREAD_STATE_RUNNING);
    int retries = use_interrupts ? 500 : 5000000;
    
    while (retries--) {
        nvme_cqe_t *cqe = &q->cq[q->cq_head];
        uint16 status = cqe->status;
        
        if ((status & 1) == q->cq_phase) {
            if (cqe->command_id == cmd_id) {
                if (cdw0) *cdw0 = cqe->command_specific;
                
                q->cq_head = (q->cq_head + 1) % NVME_QUEUE_SIZE;
                if (q->cq_head == 0) q->cq_phase ^= 1;
                
                //update CQ doorbell
                nvme_write32(ctrl, q->db_cq, q->cq_head);
                
                uint16 sc = status >> 1;
                if (sc != 0) {
                    printf("[nvme] Command failed with status 0x%x\n", sc);
                }
                return sc;
            }
        }
        
        if (use_interrupts) {
            thread_sleep(&q->wq);
        } else {
            arch_pause();
        }
    }
    return -1; //timeout
}

static int nvme_submit_cmd(nvme_ctrl_t *ctrl, nvme_queue_t *q, nvme_sqe_t *cmd, uint32 *cdw0) {
    spinlock_acquire(&q->lock);
    
    uint16 cmd_id = q->sq_tail;
    cmd->command_id = cmd_id;
    
    memcpy(&q->sq[q->sq_tail], cmd, sizeof(nvme_sqe_t));
    q->sq_tail = (q->sq_tail + 1) % NVME_QUEUE_SIZE;
    
    //update SQ doorbell
    nvme_write32(ctrl, q->db_sq, q->sq_tail);
    
    spinlock_release(&q->lock);
    
    return nvme_wait_completion(ctrl, q, cmd_id, cdw0);
}

static int nvme_submit_admin(nvme_ctrl_t *ctrl, nvme_sqe_t *cmd, uint32 *cdw0) {
    return nvme_submit_cmd(ctrl, &ctrl->admin_q, cmd, cdw0);
}

static int nvme_identify(nvme_ctrl_t *ctrl) {
    void *ptr_phys = pmm_alloc(1);
    if (!ptr_phys) return -1;
    void *ptr_virt = P2V(ptr_phys);
    memset(ptr_virt, 0, 4096);
    
    uintptr phys = (uintptr)ptr_phys;

    //identify controller
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_OP_IDENTIFY;
    cmd.cdw10 = 1; //identify controller
    cmd.prp1 = phys;
    
    if (nvme_submit_admin(ctrl, &cmd, NULL) != 0) {
        pmm_free(ptr_phys, 1);
        return -1;
    }
    
    nvme_identify_ctrl_t *id = (nvme_identify_ctrl_t *)ptr_virt;
    uint32 nn = id->nn;
    
    char model[41];
    char serial[21];
    memcpy(model, id->mn, 40); model[40] = '\0';
    memcpy(serial, id->sn, 20); serial[20] = '\0';
    nvme_trim(model, 40);
    nvme_trim(serial, 20);

    printf("[nvme] Model: %s SN: %s (Namespaces: %u)\n", model, serial, nn);
    
    ctrl->ns = kzalloc(nn * sizeof(nvme_ns_t));
    if (!ctrl->ns) {
        printf("[nvme] ERR: failed to allocate namespace array\n");
        pmm_free(ptr_phys, 1);
        return -1;
    }
    ctrl->num_ns = nn; //store total count for discovery
    
    pmm_free(ptr_phys, 1);
    return 0;
}

static int nvme_discover_namespaces(nvme_ctrl_t *ctrl) {
    void *ptr_phys = pmm_alloc(1);
    if (!ptr_phys) return -1;
    void *ptr_virt = P2V(ptr_phys);
    memset(ptr_virt, 0, 4096);
    
    uintptr phys = (uintptr)ptr_phys;
    uint32 nn = ctrl->num_ns;
    uint32 active_ns = 0;
    
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_OP_IDENTIFY;
    
    //identify all namespaces
    for (uint32 i = 1; i <= nn; i++) {
        memset(ptr_virt, 0, 4096);
        cmd.nsid = i;
        cmd.cdw10 = 0; //identify namespace
        cmd.prp1 = phys;
        
        if (nvme_submit_admin(ctrl, &cmd, NULL) != 0) continue;
        
        nvme_identify_ns_t *id_ns = (nvme_identify_ns_t *)ptr_virt;
        if (id_ns->ns_size == 0) continue; //inactive
        
        nvme_ns_t *ns = &ctrl->ns[active_ns++];
        ns->ctrl = ctrl;
        ns->nsid = i;
        ns->sector_count = id_ns->ns_size;
        
        uint8 flbas = id_ns->flbas & 0xF;
        uint32 lbaf = id_ns->lbaf[flbas];
        ns->sector_size = 1 << ((lbaf >> 16) & 0xFF);
        
        printf("[nvme] Namespace %u: %llu sectors, %u bytes/sector\n", i, ns->sector_count, ns->sector_size);

        //register object
        object_t *obj = object_create(OBJECT_DEVICE, &nvme_ops, ns);
        ns->obj = obj;
        if (obj) {
            char name[64];
            snprintf(name, sizeof(name), "$devices/disks/nvme%un%u", ctrl->ctrl_idx, i);
            ns_register(name, obj);
            printf("[nvme] Registered %s\n", name);
        }

        //scan GPT - blkdev/blkname must persist as they're used by partitions
        blkdev_t *blkdev = kzalloc(sizeof(blkdev_t));
        if (blkdev) {
            char *blkname = kzalloc(32);
            if (blkname) {
                snprintf(blkname, 32, "nvme%un%u", ctrl->ctrl_idx, i);
                blkdev->name = blkname;
                blkdev->sector_size = ns->sector_size;
                blkdev->sector_count = ns->sector_count;
                blkdev->ops = &nvme_blkdev_ops;
                blkdev->data = ns;
                gpt_scan(blkdev);
            } else {
                kfree(blkdev);
            }
        }
    }
    
    ctrl->num_ns = active_ns;
    pmm_free(ptr_phys, 1);
    return 0;
}

static int nvme_setup_io_queues(nvme_ctrl_t *ctrl) {
    //request number of queues
    nvme_sqe_t cmd = {0};
    uint32 res = 0;
    cmd.opcode = NVME_OP_SET_FEATURES;
    cmd.cdw10 = 7; //number of queues
    cmd.cdw11 = ((NVME_MAX_IO_QUEUES - 1) << 16) | (NVME_MAX_IO_QUEUES - 1);
    
    if (nvme_submit_admin(ctrl, &cmd, &res) != 0) return -1;
    
    //determine how many we actually got
    uint16 nsq = (res & 0xFFFF) + 1;
    uint16 ncq = ((res >> 16) & 0xFFFF) + 1;
    uint16 max_q = (nsq < ncq) ? nsq : ncq;
    ctrl->num_io_queues = (max_q > NVME_MAX_IO_QUEUES) ? NVME_MAX_IO_QUEUES : max_q;
    
    printf("[nvme] Setting up %u I/O queues\n", ctrl->num_io_queues);
    
    for (uint16 i = 0; i < ctrl->num_io_queues; i++) {
        nvme_queue_t *q = &ctrl->io_q[i];
        uint16 qid = i + 1;
        
        wait_queue_init(&q->wq);
        spinlock_init(&q->lock);
        q->db_sq = NVME_REG_DBL(qid, false, ctrl->dstrd);
        q->db_cq = NVME_REG_DBL(qid, true, ctrl->dstrd);
        
        //create completion queue
        void *cq_phys = pmm_alloc(1);
        q->cq = P2V(cq_phys);
        memset(q->cq, 0, 4096);
        q->cq_phase = 1;
        
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_OP_CREATE_I_CQ;
        cmd.prp1 = (uintptr)cq_phys;
        cmd.cdw10 = (NVME_QUEUE_SIZE - 1) << 16 | qid;
        cmd.cdw11 = (qid << 16) | (1 << 1) | 1; //vector qid | ien | pc
        if (nvme_submit_admin(ctrl, &cmd, NULL) != 0) return -1;
        
        //create submission queue
        void *sq_phys = pmm_alloc(1);
        q->sq = P2V(sq_phys);
        memset(q->sq, 0, 4096);
        
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_OP_CREATE_I_SQ;
        cmd.prp1 = (uintptr)sq_phys;
        cmd.cdw10 = (NVME_QUEUE_SIZE - 1) << 16 | qid;
        cmd.cdw11 = (qid << 16) | 1; //cqid | pc
        if (nvme_submit_admin(ctrl, &cmd, NULL) != 0) return -1;
    }

    return 0;
}

static int nvme_io_submit(nvme_ctrl_t *ctrl, nvme_sqe_t *cmd) {
    uint32 cpu = arch_cpu_index();
    uint16 qidx = cpu % ctrl->num_io_queues;
    return nvme_submit_cmd(ctrl, &ctrl->io_q[qidx], cmd, NULL);
}

int nvme_read(nvme_ns_t *ns, uint64 lba, uint16 count, void *buf) {
    nvme_ctrl_t *ctrl = ns->ctrl;
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_OP_READ;
    cmd.nsid = ns->nsid;
    
    uintptr phys = V2P(buf);
    cmd.prp1 = phys;
    
    //check if we need more than one PRP
    uint32 bytes = count * ns->sector_size;
    uint32 offset_in_page = (uintptr)buf & 0xFFF;
    
    void *prp_list_phys = NULL;
    if (offset_in_page + bytes > 4096) {
        //multi-page transfer
        if (offset_in_page + bytes <= 8192) {
            //exactly two pages
            cmd.prp2 = (phys & ~0xFFFULL) + 4096;
        } else {
            //more than two pages so need a PRP list
            uint32 num_pages = (offset_in_page + bytes + 4095) / 4096;
            prp_list_phys = pmm_alloc(1);
            if (!prp_list_phys) return -1;
            uint64 *prp_list = (uint64 *)P2V(prp_list_phys);
            
            for (uint32 i = 0; i < num_pages - 1; i++) {
                prp_list[i] = (phys & ~0xFFFULL) + (i + 1) * 4096;
            }
            cmd.prp2 = (uintptr)prp_list_phys;
        }
    }

    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = (lba >> 32) & 0xFFFFFFFF;
    cmd.cdw12 = (count - 1); //number of blocks (0-based)
    
    int result = nvme_io_submit(ctrl, &cmd);
    if (prp_list_phys) pmm_free(prp_list_phys, 1);
    return result;
}

int nvme_write(nvme_ns_t *ns, uint64 lba, uint16 count, const void *buf) {
    nvme_ctrl_t *ctrl = ns->ctrl;
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_OP_WRITE;
    cmd.nsid = ns->nsid;
    
    uintptr phys = V2P(buf);
    cmd.prp1 = phys;
    
    uint32 bytes = count * ns->sector_size;
    uint32 offset_in_page = (uintptr)buf & 0xFFF;
    
    void *prp_list_phys = NULL;
    if (offset_in_page + bytes > 4096) {
        if (offset_in_page + bytes <= 8192) {
            cmd.prp2 = (phys & ~0xFFFULL) + 4096;
        } else {
            uint32 num_pages = (offset_in_page + bytes + 4095) / 4096;
            prp_list_phys = pmm_alloc(1);
            if (!prp_list_phys) return -1;
            uint64 *prp_list = (uint64 *)P2V(prp_list_phys);
            for (uint32 i = 0; i < num_pages - 1; i++) {
                prp_list[i] = (phys & ~0xFFFULL) + (i + 1) * 4096;
            }
            cmd.prp2 = (uintptr)prp_list_phys;
        }
    }

    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = (lba >> 32) & 0xFFFFFFFF;
    cmd.cdw12 = (count - 1); //number of blocks (0-based)
    
    int result = nvme_io_submit(ctrl, &cmd);
    if (prp_list_phys) pmm_free(prp_list_phys, 1);
    return result;
}

//object operations
static ssize nvme_read_op(object_t *obj, void *buf, size len, size offset) {
    nvme_ns_t *ns = (nvme_ns_t *)obj->data;
    if (!ns) return -1;
    
    if (offset % ns->sector_size != 0) return -1;
    if (len % ns->sector_size != 0) return -1;
    
    uint64 lba = offset / ns->sector_size;
    uint32 count = len / ns->sector_size;
    
    if (lba + count > ns->sector_count) return -1;
    if (count > 0xFFFF) return -1; 
    
    //allocate kernel bounce buffer for DMA
    void *kbuf_phys = pmm_alloc((len + 4095) / 4096);
    if (!kbuf_phys) return -1;
    void *kbuf = P2V(kbuf_phys);
    
    int result = nvme_read(ns, lba, (uint16)count, kbuf);
    if (result == 0) {
        memcpy(buf, kbuf, len);  //copy to userspace
    }
    
    pmm_free(kbuf_phys, (len + 4095) / 4096);
    return (result == 0) ? (ssize)len : -1;
}

static ssize nvme_write_op(object_t *obj, const void *buf, size len, size offset) {
    nvme_ns_t *ns = (nvme_ns_t *)obj->data;
    if (!ns) return -1;
    
    if (offset % ns->sector_size != 0) return -1;
    if (len % ns->sector_size != 0) return -1;
    
    uint64 lba = offset / ns->sector_size;
    uint32 count = len / ns->sector_size;
    
    if (lba + count > ns->sector_count) return -1;
    if (count > 0xFFFF) return -1; 

    //allocate kernel bounce buffer for DMA
    void *kbuf_phys = pmm_alloc((len + 4095) / 4096);
    if (!kbuf_phys) return -1;
    void *kbuf = P2V(kbuf_phys);
    
    memcpy(kbuf, buf, len);  //copy from userspace
    int result = nvme_write(ns, lba, (uint16)count, kbuf);
    
    pmm_free(kbuf_phys, (len + 4095) / 4096);
    return (result == 0) ? (ssize)len : -1;
}

static int nvme_stat(object_t *obj, stat_t *st) {
    nvme_ns_t *ns = (nvme_ns_t *)obj->data;
    if (!ns || !st) return -1;
    memset(st, 0, sizeof(stat_t));
    st->type = FS_TYPE_DEVICE;
    st->size = ns->sector_count * ns->sector_size;
    return 0;
}

static int nvme_enable_msix(nvme_ctrl_t *ctrl) {
    pci_device_t *pci = ctrl->pci;
    if (!(pci->status & (1 << 4))) return -1;

    uint8 ptr = pci_config_read(pci->bus, pci->dev, pci->func, 0x34, 1);
    while (ptr) {
        uint8 cap_id = pci_config_read(pci->bus, pci->dev, pci->func, ptr, 1);
        if (cap_id == PCI_CAP_ID_MSIX) {
            ctrl->msix_cap_ptr = ptr;
            break;
        }
        ptr = pci_config_read(pci->bus, pci->dev, pci->func, ptr + 1, 1);
    }

    if (!ctrl->msix_cap_ptr) return -1;

    uint32 table_info = pci_config_read(pci->bus, pci->dev, pci->func, ctrl->msix_cap_ptr + 4, 4);
    uint8 bir = table_info & 0x7;
    uint32 offset = table_info & ~0x7;

    uint64 phys = pci->bar[bir].addr + offset;
    ctrl->msix_table = (msix_table_entry_t *)P2V(phys);
    vmm_kernel_map((uintptr)ctrl->msix_table, phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    uint32 num_vectors = 1 + NVME_MAX_IO_QUEUES;
    for (uint32 i = 0; i < num_vectors; i++) {
        ctrl->msix_table[i].msg_addr_low = 0xFEE00000;
        ctrl->msix_table[i].msg_addr_high = 0;
        ctrl->msix_table[i].msg_data = 0x40 + i;
        ctrl->msix_table[i].vector_control = 0; //unmask
    }

    uint16 msg_ctrl = pci_config_read(pci->bus, pci->dev, pci->func, ctrl->msix_cap_ptr + 2, 2);
    pci_config_write(pci->bus, pci->dev, pci->func, ctrl->msix_cap_ptr + 2, 2, msg_ctrl | (1 << 15));

    printf("[nvme] MSI-X enabled (%u vectors starting at 0x40)\n", num_vectors);
    return 0;
}

//blkdev wrappers for GPT
static int nvme_blkdev_read(blkdev_t *dev, uint64 lba, uint32 count, void *buf) {
    nvme_ns_t *ns = (nvme_ns_t *)dev->data;
    
    while (count > 0) {
        uint32 chunk = count > 0xFFFF ? 0xFFFF : count;
        int res = nvme_read(ns, lba, (uint16)chunk, buf);
        if (res != 0) return res;
        
        lba += chunk;
        count -= chunk;
        buf = (uint8 *)buf + chunk * ns->sector_size;
    }
    return 0;
}

static int nvme_blkdev_write(blkdev_t *dev, uint64 lba, uint32 count, const void *buf) {
    nvme_ns_t *ns = (nvme_ns_t *)dev->data;
    
    while (count > 0) {
        uint32 chunk = count > 0xFFFF ? 0xFFFF : count;
        int res = nvme_write(ns, lba, (uint16)chunk, buf);
        if (res != 0) return res;
        
        lba += chunk;
        count -= chunk;
        buf = (const uint8 *)buf + chunk * ns->sector_size;
    }
    return 0;
}

static void nvme_init_ctrl(pci_device_t *pci) {
    if (ctrl_count >= 4) return;
    
    nvme_ctrl_t *ctrl = kzalloc(sizeof(nvme_ctrl_t));
    if (!ctrl) return;
    
    ctrl->pci = pci;
    ctrl->ctrl_idx = ctrl_count;
    pci_enable_mmio(pci);
    pci_enable_bus_master(pci);
    
    uint64 phys = pci->bar[0].addr;
    size bar_size = pci->bar[0].size;
    ctrl->regs = (void *)(phys + HHDM_OFFSET);
    
    size pages = (bar_size + 4095) / 4096;
    vmm_kernel_map((uintptr)ctrl->regs, phys, pages, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);
    
    printf("[nvme] Initializing controller at %p (Phys: 0x%llx)\n", ctrl->regs, phys);
    
    uint32 cc = nvme_read32(ctrl, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(ctrl, NVME_REG_CC, cc & ~NVME_CC_EN);
    }
    
    int timeout = 2000000;
    while ((nvme_read32(ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) && --timeout) arch_pause();
    
    uint64 cap = nvme_read64(ctrl, NVME_REG_CAP);
    ctrl->dstrd = (cap >> 32) & 0xF;
    
    void *asq_phys = pmm_alloc(1);
    void *acq_phys = pmm_alloc(1);
    memset(P2V(asq_phys), 0, 4096);
    memset(P2V(acq_phys), 0, 4096);
    
    ctrl->admin_q.sq = P2V(asq_phys);
    ctrl->admin_q.cq = P2V(acq_phys);
    ctrl->admin_q.cq_phase = 1;
    wait_queue_init(&ctrl->admin_q.wq);
    spinlock_init(&ctrl->admin_q.lock);
    
    ctrl->admin_q.db_sq = NVME_REG_DBL(0, false, ctrl->dstrd);
    ctrl->admin_q.db_cq = NVME_REG_DBL(0, true, ctrl->dstrd);
    
    nvme_write64(ctrl, NVME_REG_ASQ, (uintptr)asq_phys);
    nvme_write64(ctrl, NVME_REG_ACQ, (uintptr)acq_phys);
    nvme_write32(ctrl, NVME_REG_AQA, (NVME_QUEUE_SIZE - 1) << 16 | (NVME_QUEUE_SIZE - 1));
    
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(ctrl, NVME_REG_CC, cc);
    
    timeout = 2000000;
    uint32 csts;
    while (!((csts = nvme_read32(ctrl, NVME_REG_CSTS)) & NVME_CSTS_RDY) && --timeout) arch_pause();
    if (timeout <= 0) {
        printf("[nvme] ERR: Controller failed to become ready (CSTS=0x%x)\n", csts);
        return;
    }
    
    if (nvme_identify(ctrl) != 0) return;
    nvme_enable_msix(ctrl);
    if (nvme_setup_io_queues(ctrl) != 0) return;
    
    object_t *disks_dir = ns_lookup("$devices/disks");
    if (!disks_dir) {
        ns_register("$devices/disks", ns_create_dir("$devices/disks/"));
    } else {
        object_deref(disks_dir);
    }

    if (nvme_discover_namespaces(ctrl) != 0) return;

    ctrls[ctrl_count++] = ctrl;
    printf("[nvme] Controller %u initialized successfully\n", ctrl->ctrl_idx);
}

void nvme_init(void) {
    pci_device_t *pdev = pci_get_devices();
    while (pdev) {
        if (pdev->class_code == 0x01 && pdev->subclass == 0x08) {
            nvme_init_ctrl(pdev);
        }
        pdev = pdev->next;
    }
}

DECLARE_DRIVER(nvme_init, INIT_LEVEL_DEVICE);
