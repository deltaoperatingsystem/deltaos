#include <drivers/xhci.h>
#include <drivers/usb.h>
#include <drivers/hid.h>
#include <drivers/pci.h>
#include <drivers/init.h>
#include <fs/fs.h>
#include <fs/tmpfs.h>
#include <obj/handle.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <arch/irq.h>
#include <lib/io.h>
#include <errno.h>
#include <lib/time.h>
#include <lib/string.h>
#include <proc/thread.h>
#include <proc/wait.h>

//up to 4 xHCI controllers
#define XHCI_MAX_CTRLS 4
static xhci_ctrl_t *g_ctrls[XHCI_MAX_CTRLS];
static uint32       g_ctrl_count = 0;
//track which BAR0 physical addresses are already initialised (dedup guard)
static uint64       g_ctrl_bar[XHCI_MAX_CTRLS];

#define RENESAS_FW_PATH             "/system/firmware/renesas_usb_fw.mem"
#define RENESAS_FW_MIN_SIZE         0x1000
#define RENESAS_FW_MAX_SIZE         0x10000
#define RENESAS_FW_VERSION          0x6C
#define RENESAS_FW_STATUS           0xF4
#define RENESAS_FW_STATUS_MSB       0xF5
#define RENESAS_ROM_STATUS          0xF6
#define RENESAS_ROM_STATUS_MSB      0xF7
#define RENESAS_DATA0               0xF8
#define RENESAS_DATA1               0xFC

#define RENESAS_FW_STATUS_DOWNLOAD_ENABLE (1 << 0)
#define RENESAS_FW_STATUS_LOCK            (1 << 1)
#define RENESAS_FW_STATUS_RESULT_MASK     (0x7 << 4)
#define RENESAS_FW_STATUS_SUCCESS         (1 << 4)
#define RENESAS_FW_STATUS_ERROR           (1 << 5)
#define RENESAS_FW_STATUS_SET_DATA0       (1 << 0)
#define RENESAS_FW_STATUS_SET_DATA1       (1 << 1)

#define RENESAS_ROM_STATUS_ACCESS         (1 << 0)
#define RENESAS_ROM_STATUS_ERASE          (1 << 1)
#define RENESAS_ROM_STATUS_RELOAD         (1 << 2)
#define RENESAS_ROM_STATUS_RESULT_MASK    (0x7 << 4)
#define RENESAS_ROM_STATUS_SUCCESS        (1 << 4)
#define RENESAS_ROM_STATUS_ERROR          (1 << 5)
#define RENESAS_ROM_STATUS_SET_DATA0      (1 << 0)
#define RENESAS_ROM_STATUS_SET_DATA1      (1 << 1)
#define RENESAS_ROM_STATUS_ROM_EXISTS     (1 << 15)

#define RENESAS_ROM_ERASE_MAGIC           0x5A65726F
#define RENESAS_ROM_WRITE_MAGIC           0x53524F4D
#define RENESAS_RETRY                     50000
#define RENESAS_CHIP_ERASE_RETRY          500000
#define RENESAS_DELAY                     10

//forward declarations
static void xhci_drain_events(xhci_ctrl_t *c);
static void xhci_ack_events(xhci_ctrl_t *c);
static void xhci_process_events(xhci_ctrl_t *c);
static void xhci_process_disconnects(xhci_ctrl_t *c);
static uint32 xhci_poll_pending_hid(xhci_ctrl_t *c);
static void xhci_recover_hid_endpoints(xhci_ctrl_t *c);
static void xhci_queue_intr(xhci_ctrl_t *c, xhci_device_t *dev);
static void xhci_init_quirks(xhci_ctrl_t *c, pci_device_t *pci);
static int xhci_claim_bios_ownership(xhci_ctrl_t *c, uint32 hccparams1);

//register accessors
static inline uint8 cap_read8(xhci_ctrl_t *c, uint32 off) {
    return *(volatile uint8 *)((uint8 *)c->cap_base + off);
}
static inline uint32 cap_read32(xhci_ctrl_t *c, uint32 off) {
    return *(volatile uint32 *)((uint8 *)c->cap_base + off);
}
static inline void cap_write32(xhci_ctrl_t *c, uint32 off, uint32 val) {
    *(volatile uint32 *)((uint8 *)c->cap_base + off) = val;
}

static inline uint32 op_read32(xhci_ctrl_t *c, uint32 off) {
    return *(volatile uint32 *)((uint8 *)c->op_base + off);
}
static inline void op_write32(xhci_ctrl_t *c, uint32 off, uint32 val) {
    *(volatile uint32 *)((uint8 *)c->op_base + off) = val;
}
static inline uint64 op_read64(xhci_ctrl_t *c, uint32 off) {
    uint32 lo = *(volatile uint32 *)((uint8 *)c->op_base + off);
    uint32 hi = *(volatile uint32 *)((uint8 *)c->op_base + off + 4);
    return ((uint64)hi << 32) | lo;
}
static inline void op_write64(xhci_ctrl_t *c, uint32 off, uint64 val) {
    *(volatile uint32 *)((uint8 *)c->op_base + off)     = (uint32)(val & 0xFFFFFFFF);
    *(volatile uint32 *)((uint8 *)c->op_base + off + 4) = (uint32)(val >> 32);
}

static inline uint32 rt_read32(xhci_ctrl_t *c, uint32 off) {
    return *(volatile uint32 *)((uint8 *)c->rt_base + off);
}
static inline void rt_write32(xhci_ctrl_t *c, uint32 off, uint32 val) {
    *(volatile uint32 *)((uint8 *)c->rt_base + off) = val;
}
static inline void rt_write64(xhci_ctrl_t *c, uint32 off, uint64 val) {
    *(volatile uint32 *)((uint8 *)c->rt_base + off)     = (uint32)(val & 0xFFFFFFFF);
    *(volatile uint32 *)((uint8 *)c->rt_base + off + 4) = (uint32)(val >> 32);
}

static inline void db_write(xhci_ctrl_t *c, uint8 slot, uint8 target) {
    c->db_base[slot] = (uint32)target;
}

//ensure TRBs/context writes are globally visible before ringing MMIO doorbells
static inline void db_ring(xhci_ctrl_t *c, uint8 slot, uint8 target) {
    arch_wmb();
    db_write(c, slot, target);
}

//context field accessors (handles 32 vs 64 byte context entries)

//get a pointer to a context block within a raw context page
//in_ctx layout: [ICC][slot][EP0..EP30], each block = ctx_size bytes
//out_ctx layout: [slot][EP0..EP30]
static inline uint32 *ctx_block(void *base, uint8 ctx_size, uint32 block_idx) {
    return (uint32 *)((uint8 *)base + (uint32)ctx_size * block_idx);
}

//write a dword at a specific dword-offset within a context block
static inline void ctx_wr(uint32 *blk, uint32 dw_off, uint32 val) {
    blk[dw_off] = val;
}
static inline uint32 ctx_rd(uint32 *blk, uint32 dw_off) {
    return blk[dw_off];
}

//ring management

//allocate a single-segment TRB ring of 'size' entries
//the last entry is always a link TRB with TC=1 (toggle cycle)
static void ring_reinit(xhci_ring_t *ring) {
    if (!ring || !ring->trbs || ring->size < 2) return;

    uint32 bytes = ring->size * (uint32)sizeof(xhci_trb_t);
    memset(ring->trbs, 0, bytes);

    ring->enq = 0;
    ring->deq = 0;
    ring->pcs = 1;

    xhci_trb_t *link = &ring->trbs[ring->size - 1];
    link->param   = (uint64)ring->phys;
    link->status  = 0;
    link->control = ((uint32)TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | TRB_C;
    if (ring->chain_links) {
        link->control |= TRB_CH;
    }
}

static int ring_alloc(xhci_ring_t *ring, uint32 size, bool chain_links) {
    uint32 bytes = size * (uint32)sizeof(xhci_trb_t);
    uint32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc(pages);
    if (!phys) return -1;

    ring->trbs = (xhci_trb_t *)P2V(phys);
    ring->phys = (uintptr)phys;
    ring->size = size;
    ring->pages = pages;
    ring->chain_links = chain_links;
    ring_reinit(ring);

    return 0;
}

static void ring_free(xhci_ring_t *ring) {
    if (!ring) return;
    if (ring->trbs && ring->pages) {
        pmm_free((void *)ring->phys, ring->pages);
    }
    memset(ring, 0, sizeof(*ring));
}

//enqueue one TRB onto a producer ring (command or transfer rin
//returns the physical address of the enqueued TRB (used to match completions)
static uintptr ring_enqueue(xhci_ring_t *ring,
                             uint64 param, uint32 status, uint32 flags) {
    //stamp cycle bit
    uint32 control = (flags & ~TRB_C) | (ring->pcs ? TRB_C : 0);

    xhci_trb_t *trb = &ring->trbs[ring->enq];
    trb->param   = param;
    trb->status  = status;
    trb->control = control;

    uintptr phys = ring->phys + ring->enq * sizeof(xhci_trb_t);

    ring->enq++;

    //if we've hit the link TRB, update its cycle bit, toggle PCS, wrap
    if (ring->enq == ring->size - 1) {
        xhci_trb_t *link = &ring->trbs[ring->size - 1];
        //link TRB cycle bit must equal current PCS so HC follows it
        if (ring->pcs) link->control |= TRB_C;
        else           link->control &= ~TRB_C;
        ring->pcs ^= 1;
        ring->enq = 0;
    }

    return phys;
}

//advance the event ring dequeue pointer by one and toggle CCS on wrap
static void evt_advance(xhci_ctrl_t *c) {
    c->evt_deq++;
    if (c->evt_deq == XHCI_EVT_RING_SIZE) {
        c->evt_deq = 0;
        c->evt_ccs ^= 1;
    }
}

//tell the controller where we have consumed up to in the event rin.
static void evt_update_erdp(xhci_ctrl_t *c) {
    uint64 erdp = c->evt_ring_phys + c->evt_deq * sizeof(xhci_trb_t);
    erdp |= ERDP_EHB;   //write 1 to clear the EHB / pending flag
    rt_write64(c, XHCI_RT_ERDP(0), erdp);
}


//MSI-X setup
static int xhci_enable_msix(xhci_ctrl_t *c) {
    pci_device_t *pci = c->pci;

    //status register bit 4 = capabilities list present
    if (!(pci->status & (1 << 4))) return -1;

    //walk the PCI capability list looking for MSI-X (cap ID 0x11)
    uint8 ptr = pci_config_read(pci->bus, pci->dev, pci->func, 0x34, 1);
    while (ptr) {
        uint8 id = pci_config_read(pci->bus, pci->dev, pci->func, ptr, 1);
        if (id == PCI_CAP_MSIX) {
            c->msix_cap = ptr;
            break;
        }
        ptr = pci_config_read(pci->bus, pci->dev, pci->func, ptr + 1, 1);
    }
    if (!c->msix_cap) return -1;

    //MSI-X table: cap+4 → [BIR:2:0] = BAR index, [31:3] = table offset
    uint32 tbl_info = pci_config_read(pci->bus, pci->dev, pci->func, c->msix_cap + 4, 4);
    uint8  bir    = tbl_info & 0x7;
    uint32 offset = tbl_info & ~0x7U;

    uint64 tbl_phys = pci->bar[bir].addr + offset;
    c->msix_table   = (xhci_msix_entry_t *)P2V(tbl_phys);
    vmm_kernel_map((uintptr)c->msix_table, tbl_phys, 1, MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    //compose MSI message
    irq_msi_msg_t msg;
    if (irq_compose_msi(XHCI_MSI_VECTOR, &msg) < 0) return -1;

    c->msix_table[0].msg_addr_lo = msg.addr_lo;
    c->msix_table[0].msg_addr_hi = msg.addr_hi;
    c->msix_table[0].msg_data    = msg.data;
    c->msix_table[0].vector_ctrl = 0; //unmask

    //enable MSI-X in the capability (bit 15 of message control)
    uint16 msgctl = pci_config_read(pci->bus, pci->dev, pci->func, c->msix_cap + 2, 2);
    pci_config_write(pci->bus, pci->dev, pci->func, c->msix_cap + 2, 2, msgctl | (1 << 15));

    printf("[xhci] MSI-X enabled (vector 0x%02X)\n", XHCI_MSI_VECTOR);
    return 0;
}

static int xhci_enable_msi(xhci_ctrl_t *c) {
    pci_device_t *pci = c->pci;
    if (!(pci->status & (1 << 4))) return -1;

    uint8 ptr = pci_config_read(pci->bus, pci->dev, pci->func, 0x34, 1);
    uint8 msi_cap = 0;
    while (ptr) {
        uint8 id = pci_config_read(pci->bus, pci->dev, pci->func, ptr, 1);
        if (id == 0x05) { //PCI_CAP_MSI
            msi_cap = ptr;
            break;
        }
        ptr = pci_config_read(pci->bus, pci->dev, pci->func, ptr + 1, 1);
    }
    if (!msi_cap) return -1;

    irq_msi_msg_t msg;
    if (irq_compose_msi(XHCI_MSI_VECTOR, &msg) < 0) return -1;

    uint16 msgctl = pci_config_read(pci->bus, pci->dev, pci->func, msi_cap + 2, 2);
    bool is_64bit = (msgctl & (1 << 7)) != 0;

    pci_config_write(pci->bus, pci->dev, pci->func, msi_cap + 4, 4, msg.addr_lo);
    if (is_64bit) {
        pci_config_write(pci->bus, pci->dev, pci->func, msi_cap + 8, 4, msg.addr_hi);
        pci_config_write(pci->bus, pci->dev, pci->func, msi_cap + 12, 2, msg.data);
    } else {
        pci_config_write(pci->bus, pci->dev, pci->func, msi_cap + 8, 2, msg.data);
    }

    //enable MSI (bit 0)
    pci_config_write(pci->bus, pci->dev, pci->func, msi_cap + 2, 2, msgctl | 1);

    printf("[xhci] MSI enabled (vector 0x%02X)\n", XHCI_MSI_VECTOR);
    return 0;
}

static int xhci_enable_interrupts(xhci_ctrl_t *c) {
    if (xhci_enable_msix(c) == 0) return 0;
    if (xhci_enable_msi(c) == 0) return 0;
    return -1;
}

static bool xhci_renesas_fw_probe(pci_device_t *pci, uint32 *fw_version,
                                  uint16 *fw_status, uint16 *rom_status) {
    if (!pci || pci->vendor_id != 0x1033) return false;

    if (fw_version) {
        *fw_version = pci_config_read(pci->bus, pci->dev, pci->func, 0x6C, 4);
    }
    if (fw_status) {
        *fw_status = (uint16)pci_config_read(pci->bus, pci->dev, pci->func, 0xF4, 2);
    }
    if (rom_status) {
        *rom_status = (uint16)pci_config_read(pci->bus, pci->dev, pci->func, 0xF6, 2);
    }
    return true;
}

static bool xhci_renesas_check_rom(pci_device_t *pci) {
    uint16 rom_status = 0;
    if (!xhci_renesas_fw_probe(pci, NULL, NULL, &rom_status)) {
        return false;
    }
    return (rom_status & RENESAS_ROM_STATUS_ROM_EXISTS) != 0;
}

static int xhci_renesas_check_rom_state(pci_device_t *pci) {
    uint32 fw_version = 0;
    uint16 fw_status = 0;
    uint16 rom_status = 0;

    if (!xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
        return -1;
    }

    if ((rom_status & RENESAS_ROM_STATUS_ROM_EXISTS) &&
        ((rom_status & RENESAS_ROM_STATUS_RESULT_MASK) == RENESAS_ROM_STATUS_SUCCESS)) {
        return 0;
    }

    (void)fw_version;

    if (fw_status & RENESAS_FW_STATUS_LOCK) {
        if (fw_status & RENESAS_FW_STATUS_SUCCESS) return 0;
        return -EIO;
    }

    if (fw_status & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) {
        return -EIO;
    }

    switch (fw_status & RENESAS_FW_STATUS_RESULT_MASK) {
        case 0:
            return 1;
        case RENESAS_FW_STATUS_SUCCESS:
            return 0;
        case RENESAS_FW_STATUS_ERROR:
            return -ENODEV;
        default:
            return -EINVAL;
    }
}

static int xhci_renesas_check_running(pci_device_t *pci) {
    int rom = xhci_renesas_check_rom_state(pci);
    if (rom == 0) return 0;
    if (rom < 0 && rom != 1) return rom;

    uint32 fw_version = 0;
    uint16 fw_status = 0;
    uint16 rom_status = 0;
    if (!xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
        return -1;
    }

    (void)fw_version;
    (void)rom_status;

    if (fw_status & RENESAS_FW_STATUS_LOCK) {
        if (fw_status & RENESAS_FW_STATUS_SUCCESS) return 0;
        return -EIO;
    }

    if (fw_status & RENESAS_FW_STATUS_DOWNLOAD_ENABLE) {
        return -EIO;
    }

    switch (fw_status & RENESAS_FW_STATUS_RESULT_MASK) {
        case 0:
            return 1;
        case RENESAS_FW_STATUS_SUCCESS:
            return 0;
        case RENESAS_FW_STATUS_ERROR:
            return -ENODEV;
        default:
            return -EINVAL;
    }
}

static int xhci_renesas_fw_verify_blob(const uint8 *fw, size len) {
    if (!fw || len < RENESAS_FW_MIN_SIZE || len >= RENESAS_FW_MAX_SIZE) {
        return -EINVAL;
    }

    if ((uint16)fw[0] != 0xAA || (uint16)fw[1] != 0x55) {
        return -EINVAL;
    }

    uint16 version_ptr = (uint16)fw[4] | ((uint16)fw[5] << 8);
    if ((size)version_ptr + 2 >= len) {
        return -EINVAL;
    }

    return 0;
}

static int xhci_renesas_fw_read_blob(const char *path, uint8 **fw_out, size *fw_len) {
    if (!path || !fw_out || !fw_len) return -EINVAL;

    stat_t st;
    if (handle_stat(path, &st) < 0 || st.type != FS_TYPE_FILE) {
        return -ENOENT;
    }

    if (st.size < RENESAS_FW_MIN_SIZE || st.size >= RENESAS_FW_MAX_SIZE ||
        (st.size & 3) != 0) {
        return -EINVAL;
    }

    object_t *file = tmpfs_open(path);
    if (!file) return -ENOENT;

    uint8 *buf = kmalloc(st.size);
    if (!buf) {
        object_release(file);
        return -ENOMEM;
    }

    ssize got = object_read(file, buf, st.size, 0);
    object_release(file);

    if (got != (ssize)st.size) {
        kfree(buf);
        return -EIO;
    }

    *fw_out = buf;
    *fw_len = st.size;
    return 0;
}

static int xhci_renesas_fw_download_image(pci_device_t *pci, const uint32 *fw,
                                          size step, bool rom) {
    uint32 status_reg = rom ? RENESAS_ROM_STATUS_MSB : RENESAS_FW_STATUS_MSB;
    bool data1 = (step & 1) != 0;
    uint32 bit = data1 ? RENESAS_FW_STATUS_SET_DATA1 : RENESAS_FW_STATUS_SET_DATA0;

    for (size i = 0; i < 50000; i++) {
        uint8 fw_status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                                 (uint16)status_reg, 1);
        if (!(fw_status & (uint8)bit)) break;
        usleep(10);
    }

    uint32 data_reg = data1 ? RENESAS_DATA1 : RENESAS_DATA0;
    uint32 data = fw[step];
    pci_config_write(pci->bus, pci->dev, pci->func, (uint16)data_reg, 4, data);
    usleep(100);
    pci_config_write(pci->bus, pci->dev, pci->func, (uint16)status_reg, 1, bit);

    return 0;
}

static int xhci_renesas_fw_download(pci_device_t *pci, const uint8 *fw, size len) {
    if (!pci || !fw || len < RENESAS_FW_MIN_SIZE) return -EINVAL;

    const uint32 *fw_words = (const uint32 *)fw;
    size word_count = len / 4;

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_FW_STATUS, 1,
                     RENESAS_FW_STATUS_DOWNLOAD_ENABLE);

    for (size i = 0; i < word_count; i++) {
        int err = xhci_renesas_fw_download_image(pci, fw_words, i, false);
        if (err) return err;
    }

    for (size i = 0; i < 50000; i++) {
        uint8 fw_status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                                 RENESAS_FW_STATUS_MSB, 1);
        if (!(fw_status & (RENESAS_FW_STATUS_SET_DATA0 | RENESAS_FW_STATUS_SET_DATA1)))
            break;
        usleep(10);
    }

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_FW_STATUS, 1, 0);

    for (size i = 0; i < 1000; i++) {
        uint16 fw_status = (uint16)pci_config_read(pci->bus, pci->dev, pci->func,
                                                   RENESAS_FW_STATUS, 1);
        if ((fw_status & RENESAS_FW_STATUS_LOCK) &&
            (fw_status & RENESAS_FW_STATUS_SUCCESS)) {
            return 0;
        }
        if ((fw_status & RENESAS_FW_STATUS_RESULT_MASK) == RENESAS_FW_STATUS_SUCCESS) {
            return 0;
        }
        usleep(100);
    }

    return -ETIMEDOUT;
}

static void xhci_renesas_rom_erase(pci_device_t *pci) {
    if (!pci) return;

    printf("[xhci] Renesas ROM erase\n");
    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_DATA0, 4,
                     RENESAS_ROM_ERASE_MAGIC);

    uint8 status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                          RENESAS_ROM_STATUS, 1);
    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1,
                     status | RENESAS_ROM_STATUS_ERASE);

    sleep(20);
    for (size i = 0; i < RENESAS_CHIP_ERASE_RETRY; i++) {
        status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                        RENESAS_ROM_STATUS, 1);
        if (!(status & RENESAS_ROM_STATUS_ERASE)) break;
        usleep(RENESAS_DELAY);
    }
}

static bool xhci_renesas_setup_rom(pci_device_t *pci, const uint8 *fw, size len) {
    if (!pci || !fw || len < RENESAS_FW_MIN_SIZE) return false;

    const uint32 *fw_words = (const uint32 *)fw;
    size word_count = len / 4;

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_DATA0, 4,
                     RENESAS_ROM_WRITE_MAGIC);
    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1,
                     RENESAS_ROM_STATUS_ACCESS);

    uint8 status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                          RENESAS_ROM_STATUS, 1);
    if (status & RENESAS_ROM_STATUS_RESULT_MASK) {
        pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
        return false;
    }

    for (size i = 0; i < word_count; i++) {
        if (xhci_renesas_fw_download_image(pci, fw_words, i, true) < 0) {
            pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
            return false;
        }
    }

    for (size i = 0; i < RENESAS_RETRY; i++) {
        status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                        RENESAS_ROM_STATUS_MSB, 1);
        if (!(status & (RENESAS_ROM_STATUS_SET_DATA0 | RENESAS_ROM_STATUS_SET_DATA1)))
            break;
        usleep(RENESAS_DELAY);
    }

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
    usleep(10);

    for (size i = 0; i < RENESAS_RETRY; i++) {
        status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                        RENESAS_ROM_STATUS, 1);
        status &= RENESAS_ROM_STATUS_RESULT_MASK;
        if (status == RENESAS_ROM_STATUS_SUCCESS) {
            pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1,
                             RENESAS_ROM_STATUS_RELOAD);
            for (size j = 0; j < RENESAS_RETRY; j++) {
                status = (uint8)pci_config_read(pci->bus, pci->dev, pci->func,
                                                RENESAS_ROM_STATUS, 1);
                if (!(status & RENESAS_ROM_STATUS_RELOAD)) {
                    return true;
                }
                usleep(RENESAS_DELAY);
            }
            return false;
        }
        usleep(RENESAS_DELAY);
    }

    pci_config_write(pci->bus, pci->dev, pci->func, RENESAS_ROM_STATUS, 1, 0);
    return false;
}

static int xhci_renesas_fw_load(pci_device_t *pci) {
    if (!pci || pci->vendor_id != 0x1033 || pci->device_id != 0x0194) {
        return 0;
    }

    int running = xhci_renesas_check_running(pci);
    if (running == 0) {
        uint32 fw_version = 0;
        uint16 fw_status = 0;
        uint16 rom_status = 0;
        if (xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
            if ((rom_status & RENESAS_ROM_STATUS_ROM_EXISTS) &&
                ((rom_status & RENESAS_ROM_STATUS_RESULT_MASK) ==
                 RENESAS_ROM_STATUS_SUCCESS)) {
                printf("[xhci] Renesas ROM ready (ver=0x%08X rom=0x%04X)\n",
                       fw_version, rom_status);
            } else {
                printf("[xhci] Renesas firmware ready (ver=0x%08X fw=0x%04X rom=0x%04X)\n",
                       fw_version, fw_status, rom_status);
            }
        }
        return 0;
    }

    if (running < 0 && running != 1) {
        return 0;
    }

    uint8 *fw = NULL;
    size fw_len = 0;
    int err = xhci_renesas_fw_read_blob(RENESAS_FW_PATH, &fw, &fw_len);
    if (err) {
        printf("[xhci] Renesas firmware blob unavailable at %s (%d), continuing\n",
               RENESAS_FW_PATH, err);
        return 0;
    }

    err = xhci_renesas_fw_verify_blob(fw, fw_len);
    if (err) {
        printf("[xhci] Renesas firmware blob invalid (%d), continuing\n", err);
        kfree(fw);
        return 0;
    }

    if (xhci_renesas_check_rom(pci)) {
        xhci_renesas_rom_erase(pci);
        if (xhci_renesas_setup_rom(pci, fw, fw_len)) {
            uint32 fw_version = 0;
            uint16 fw_status = 0;
            uint16 rom_status = 0;
            if (xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
                printf("[xhci] Renesas ROM programmed (ver=0x%08X rom=0x%04X)\n",
                       fw_version, rom_status);
            }
            kfree(fw);
            return 0;
        }

        printf("[xhci] Renesas ROM programming failed, falling back to FW load\n");
    }

    uint32 fw_version = 0;
    uint16 fw_status = 0;
    uint16 rom_status = 0;
    if (xhci_renesas_fw_probe(pci, &fw_version, &fw_status, &rom_status)) {
        printf("[xhci] loading Renesas firmware (ver=0x%08X fw=0x%04X rom=0x%04X)\n",
               fw_version, fw_status, rom_status);
    }

    err = xhci_renesas_fw_download(pci, fw, fw_len);
    kfree(fw);
    if (err) {
        printf("[xhci] Renesas firmware load failed (%d), continuing\n", err);
        return 0;
    }

    return 0;
}

static uint32 xhci_find_ext_cap(xhci_ctrl_t *c, uint32 hccparams1, uint8 cap_id) {
    uint32 off = HCCPARAMS1_XECP(hccparams1) << 2;

    for (uint32 i = 0; off && i < 64; i++) {
        uint32 hdr = cap_read32(c, off);
        if ((hdr & 0xFF) == cap_id) return off;
        off = ((hdr >> 8) & 0xFFFF) << 2;
    }

    return 0;
}

static int xhci_claim_bios_ownership(xhci_ctrl_t *c, uint32 hccparams1) {
    uint32 off = xhci_find_ext_cap(c, hccparams1, XHCI_EXT_CAPS_LEGACY);
    if (!off) return 0;

    uint32 leg0 = cap_read32(c, off + XHCI_LEGACY_SUPPORT_OFFSET);
    uint32 leg1 = cap_read32(c, off + XHCI_LEGACY_CONTROL_OFFSET);

    if (!(leg0 & XHCI_HC_BIOS_OWNED) && !(c->quirks & XHCI_QUIRK_FORCE_BIOS_HANDOFF)) {
        //no firmware ownership to reclaim
        return 0;
    }

    printf("[xhci] legacy handoff: claiming OS ownership\n");
    cap_write32(c, off + XHCI_LEGACY_SUPPORT_OFFSET, leg0 | XHCI_HC_OS_OWNED);

    for (uint32 elapsed = 0; elapsed < 500; elapsed++) {
        leg0 = cap_read32(c, off + XHCI_LEGACY_SUPPORT_OFFSET);
        if (!(leg0 & XHCI_HC_BIOS_OWNED)) break;
        sleep(1);
    }

    leg0 = cap_read32(c, off + XHCI_LEGACY_SUPPORT_OFFSET);
    if (leg0 & XHCI_HC_BIOS_OWNED) {
        printf("[xhci] legacy handoff: BIOS ownership stuck, forcing clear\n");
        cap_write32(c, off + XHCI_LEGACY_SUPPORT_OFFSET, leg0 & ~XHCI_HC_BIOS_OWNED);
    }

    //clear any firwmare-generated SMI enables in the companion control dword
    cap_write32(c, off + XHCI_LEGACY_CONTROL_OFFSET, leg1 & ~XHCI_LEGACY_SMI_EVENTS);
    return 0;
}

//wait for a command completion event
static int xhci_wait_cmd(xhci_ctrl_t *c) {
    //poll
    for (uint32 elapsed = 0; elapsed < 1000; elapsed++) {
        if (c->cmd_done) goto done;
        xhci_poll_pending_hid(c);
        if (c->cmd_done) goto done;
        sleep(1);
    }

    //timed out - still do the MMIO housekeeping and report failure
    {
        irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
        xhci_ack_events(c);
        spinlock_irq_release(&c->evt_lock, fl);
    }
    return -1;
done:
    {
        irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
        xhci_ack_events(c);
        spinlock_irq_release(&c->evt_lock, fl);
    }
    return c->cmd_cc;
}

//wait for a control transfer (EP0) to complete
static int xhci_wait_ctrl_xfer(xhci_ctrl_t *c, xhci_device_t *dev) {
    for (uint32 elapsed = 0; elapsed < 1000; elapsed++) {
        if (dev->ctrl_done) goto done;
        xhci_poll_pending_hid(c);
        if (dev->ctrl_done) goto done;
        sleep(1);
    }

    {
        irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
        xhci_ack_events(c);
        spinlock_irq_release(&c->evt_lock, fl);
    }
    return -1;
done:
    {
        irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
        xhci_ack_events(c);
        spinlock_irq_release(&c->evt_lock, fl);
    }
    return dev->ctrl_cc;
}

//command submission
static int cmd_enable_slot(xhci_ctrl_t *c) {
    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    //clear done flag while holding the lock so we cannot miss a completion
    //that arrives between the doorbell write and xhci_wait_cmd
    c->cmd_done = 0;
    c->cmd_slot = 0;

    uint32 ctrl = (TRB_TYPE_ENABLE_SLOT << TRB_TYPE_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, 0, 0, ctrl);
    db_ring(c, 0, 0);  //ring host controller doorbell

    spinlock_irq_release(&c->cmd_lock, fl);
    int cc = xhci_wait_cmd(c);
    return (cc == TRB_CC_SUCCESS) ? (int)c->cmd_slot : -1;
}

static void xhci_nec_get_fw(xhci_ctrl_t *c) {
    if (!xhci_has_quirk(c, XHCI_QUIRK_NEC_HOST)) return;

    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;
    c->cmd_slot = 0;

    uint32 ctrl = (TRB_NEC_GET_FW << TRB_TYPE_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, 0, 0, ctrl);
    db_ring(c, 0, 0);

    spinlock_irq_release(&c->cmd_lock, fl);

    (void)xhci_wait_cmd(c);
}

static int cmd_disable_slot_submit(xhci_ctrl_t *c, uint8 slot) {
    if (slot < 1 || slot > XHCI_MAX_SLOTS) return -1;

    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;
    uint32 ctrl = (TRB_TYPE_DISABLE_SLOT << TRB_TYPE_SHIFT)
                | ((uint32)slot << TRB_SLOT_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, 0, 0, ctrl);
    db_ring(c, 0, 0);
    spinlock_irq_release(&c->cmd_lock, fl);
    return xhci_wait_cmd(c);
}

static int cmd_address_device(xhci_ctrl_t *c, uint8 slot,
                                uintptr in_ctx_phys, bool bsr) {    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;  //cleared under lock before enqueue

    uint32 ctrl = (TRB_TYPE_ADDR_DEVICE << TRB_TYPE_SHIFT)
                | ((uint32)slot << TRB_SLOT_SHIFT)
                | (bsr ? TRB_BSR : 0);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, in_ctx_phys, 0, ctrl);
    db_ring(c, 0, 0);

    spinlock_irq_release(&c->cmd_lock, fl);
    return xhci_wait_cmd(c);
}

static int cmd_configure_ep(xhci_ctrl_t *c, uint8 slot, uintptr in_ctx_phys) {
    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;  //cleared under lock before enqueue

    uint32 ctrl = (TRB_TYPE_CONFIG_EP << TRB_TYPE_SHIFT)
                | ((uint32)slot << TRB_SLOT_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, in_ctx_phys, 0, ctrl);
    db_ring(c, 0, 0);

    spinlock_irq_release(&c->cmd_lock, fl);
    return xhci_wait_cmd(c);
}

static int cmd_eval_ctx(xhci_ctrl_t *c, uint8 slot, uintptr in_ctx_phys) {
    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;  //cleared under lock before enqueue

    uint32 ctrl = (TRB_TYPE_EVAL_CTX << TRB_TYPE_SHIFT)
                | ((uint32)slot << TRB_SLOT_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, in_ctx_phys, 0, ctrl);
    db_ring(c, 0, 0);

    spinlock_irq_release(&c->cmd_lock, fl);
    return xhci_wait_cmd(c);
}

static int cmd_reset_ep(xhci_ctrl_t *c, uint8 slot, uint8 dci) {
    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;  //cleared under lock before enqueue

    uint32 ctrl = (TRB_TYPE_RESET_EP << TRB_TYPE_SHIFT)
                | ((uint32)dci << TRB_EP_SHIFT)
                | ((uint32)slot << TRB_SLOT_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, 0, 0, ctrl);
    db_ring(c, 0, 0);

    spinlock_irq_release(&c->cmd_lock, fl);
    return xhci_wait_cmd(c);
}

static int cmd_set_tr_deq(xhci_ctrl_t *c, uint8 slot, uint8 dci, uintptr tr_deq_phys) {
    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
    c->cmd_done = 0;  //cleared under lock before enqueue

    uint64 param = ((uint64)tr_deq_phys & ~0xFULL) | 1U;  //DCS=1
    uint32 ctrl = (TRB_TYPE_SET_TRDEQ << TRB_TYPE_SHIFT)
                | ((uint32)dci << TRB_EP_SHIFT)
                | ((uint32)slot << TRB_SLOT_SHIFT);
    c->last_cmd_phys = ring_enqueue(&c->cmd_ring, param, 0, ctrl);
    db_ring(c, 0, 0);

    spinlock_irq_release(&c->cmd_lock, fl);
    return xhci_wait_cmd(c);
}

//control transfer (EP0 - DCI 1)

//issue a USB control transfer on EP0 of 'dev'
//`setup` is the 8-byte setup packet
//`buf_phys` is the physical address of the data buffer (0 if no data stage)
//`len` is the transfer length (0 if no data stage)
//`dir_in` true = device->host (IN data stage), false = host->device
static int xhci_ctrl_xfer(xhci_ctrl_t *c, xhci_device_t *dev,
                           usb_setup_pkt_t *setup,
                           uintptr buf_phys, uint16 len, bool dir_in) {
    xhci_ring_t *ring = &dev->ep_ring[DCI_EP0 - 1];

    //pack the 8-byte setup packet into a uint64 for the TRB param field
    uint64 setup_param;
    memcpy(&setup_param, setup, 8);

    //TRT (Transfer Type) for setup stage
    uint32 trt = (len == 0) ? TRB_TRT_NO_DATA
               : (dir_in   ? TRB_TRT_IN_DATA : TRB_TRT_OUT_DATA);

    //setup stage TRB
    uint32 setup_ctrl = ((uint32)TRB_TYPE_SETUP << TRB_TYPE_SHIFT)
                      | TRB_IDT        //immediate data: setup packet is inline
                      | trt;
    ring_enqueue(ring, setup_param, 8 /*setup packet length*/, setup_ctrl);

    //data stage TRB (optional)
    if (len > 0) {
        uint32 data_ctrl = ((uint32)TRB_TYPE_DATA << TRB_TYPE_SHIFT)
                         | (dir_in ? TRB_DIR_IN : 0);
        ring_enqueue(ring, (uint64)buf_phys, (uint32)len, data_ctrl);
    }

    //status stage TRB (IOC=1 ->generates transfer event)
    //direction is opposite of data stage (or IN when no data stage)
    uint32 status_dir = (len > 0 && dir_in) ? 0 : TRB_DIR_IN;
    uint32 status_ctrl = ((uint32)TRB_TYPE_STATUS << TRB_TYPE_SHIFT)
                       | TRB_IOC
                       | status_dir;
    ring_enqueue(ring, 0, 0, status_ctrl);

    dev->ctrl_done = 0;
    //ring doorbell for slot 'slot_id', endpoint DCI 1 (target = 1)
    db_ring(c, dev->slot_id, DCI_EP0);

    return xhci_wait_ctrl_xfer(c, dev);
}

//convenience wrapper: get a descriptor
static int xhci_get_descriptor(xhci_ctrl_t *c, xhci_device_t *dev,
                                uint8 desc_type, uint8 desc_idx,
                                void *buf_virt, uintptr buf_phys, uint16 len) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_DIR_D2H | USB_TYPE_STD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16)((desc_type << 8) | desc_idx),
        .wIndex        = 0,
        .wLength       = len,
    };
    (void)buf_virt;
    return xhci_ctrl_xfer(c, dev, &setup, buf_phys, len, true);
}

static int xhci_get_hid_report_descriptor(xhci_ctrl_t *c, xhci_device_t *dev,
                                          uint8 iface, void *buf_virt,
                                          uintptr buf_phys, uint16 len) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_DIR_D2H | USB_TYPE_STD | USB_RECIP_IFACE,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16)((USB_DESC_HID_REPORT << 8) | 0),
        .wIndex        = iface,
        .wLength       = len,
    };
    (void)buf_virt;
    return xhci_ctrl_xfer(c, dev, &setup, buf_phys, len, true);
}

static int xhci_set_config(xhci_ctrl_t *c, xhci_device_t *dev, uint8 cfg_val) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_DIR_H2D | USB_TYPE_STD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_SET_CONFIG,
        .wValue        = cfg_val,
        .wIndex        = 0,
        .wLength       = 0,
    };
    return xhci_ctrl_xfer(c, dev, &setup, 0, 0, false);
}

static int xhci_hid_set_protocol(xhci_ctrl_t *c, xhci_device_t *dev,
                                  uint8 iface, uint8 proto) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_DIR_H2D | USB_TYPE_CLASS | USB_RECIP_IFACE,
        .bRequest      = USB_HID_REQ_SET_PROTO,
        .wValue        = proto,  //0 = boot, 1 = report
        .wIndex        = iface,
        .wLength       = 0,
    };
    return xhci_ctrl_xfer(c, dev, &setup, 0, 0, false);
}

static int xhci_hid_set_idle(xhci_ctrl_t *c, xhci_device_t *dev,
                              uint8 iface, uint8 duration) {
    usb_setup_pkt_t setup = {
        .bmRequestType = USB_DIR_H2D | USB_TYPE_CLASS | USB_RECIP_IFACE,
        .bRequest      = USB_HID_REQ_SET_IDLE,
        .wValue        = (uint16)((duration << 8) | 0), //report ID 0
        .wIndex        = iface,
        .wLength       = 0,
    };
    return xhci_ctrl_xfer(c, dev, &setup, 0, 0, false);
}

//interrupt transfer (for HID polling)

//queue one normal TRB on a HID interrupt IN endpoint
//when the transfer completes the HC writes a transfer event which
//xhci_process_events() will pick up and dispatch to hid_report_received()
static void xhci_queue_intr(xhci_ctrl_t *c, xhci_device_t *dev) {
    if (!dev || !dev->is_hid || dev->hid_ep_dci == 0) return;
    if (dev->hid_recovering) return;
    if (dev->hid_intr_queued) return;

    xhci_ring_t *ring = &dev->ep_ring[dev->hid_ep_dci - 1];
    uint32 ctrl = ((uint32)TRB_TYPE_NORMAL << TRB_TYPE_SHIFT)
                | TRB_IOC | TRB_ISP;
    ring_enqueue(ring, (uint64)(uintptr)dev->hid_buf_phys,
                 dev->hid_ep_mps, ctrl);
    db_ring(c, dev->slot_id, dev->hid_ep_dci);
    dev->hid_intr_queued = true;
}

//input context helpers

//allocate one page for an input context and zero it
//returns virtual pointer; *phys_out receives physical address
static void *alloc_input_ctx(xhci_ctrl_t *c, uintptr *phys_out) {
    void *phys = pmm_alloc(1);
    if (!phys) return NULL;
    *phys_out = (uintptr)phys;
    void *virt = P2V(phys);
    memset(virt, 0, PAGE_SIZE);
    (void)c;
    return virt;
}

//build the input context for address device (slot + EP0 only)
static void build_addr_device_ctx(xhci_ctrl_t *c, xhci_device_t *dev,
                                   void *in_ctx) {
    uint8 cs = c->ctx_size;

    //block 0 = input control context: A0 (slot) + A1 (EP0)
    uint32 *icc = ctx_block(in_ctx, cs, 0);
    ctx_wr(icc, 0, 0);         //drop flags
    ctx_wr(icc, 1, 0x3);       //add slot (bit 0) + EP0 (bit 1)

    //block 1 = slot context
    uint32 *slot = ctx_block(in_ctx, cs, 1);
    //DW0: speed [23:20], Context Entries [31:27] = 1
    ctx_wr(slot, 0, ((uint32)dev->speed << 20) | (1U << 27));
    //DW1: Root Hub Port Number [23:16] (1-based)
    ctx_wr(slot, 1, (uint32)(dev->port + 1) << 16);
    ctx_wr(slot, 2, 0);
    ctx_wr(slot, 3, 0);

    //block 2 = EP0 context (DCI 1)
    uint32 *ep0 = ctx_block(in_ctx, cs, 2);
    //DW0: Interval = 0
    ctx_wr(ep0, 0, 0);
    //DW1: CErr=3, EP Type=4 (control bidir), max  packet size
    ctx_wr(ep0, 1, (3U << 1) | ((uint32)EP_TYPE_CTRL << 3) | ((uint32)dev->mps0 << 16));
    //DW2+DW3: TR dequeue pointer | DCS=1
    uintptr ep0_ring_phys = dev->ep_ring[DCI_EP0 - 1].phys;
    ctx_wr(ep0, 2, (uint32)((ep0_ring_phys & ~0xFULL) | 1));
    ctx_wr(ep0, 3, (uint32)(ep0_ring_phys >> 32));
    //DW4: average TRB length = 8 for control
    ctx_wr(ep0, 4, 8);
}

//build a evaluate context to update EP0 max packet size after GET_DESCRIPTOR
static void build_eval_mps_ctx(xhci_ctrl_t *c, xhci_device_t *dev, void *in_ctx) {
    uint8 cs = c->ctx_size;
    uint32 *icc = ctx_block(in_ctx, cs, 0);
    ctx_wr(icc, 0, 0);
    ctx_wr(icc, 1, 0x2);    //add EP0 only (bit 1)

    uint32 *ep0 = ctx_block(in_ctx, cs, 2);
    ctx_wr(ep0, 0, 0);
    ctx_wr(ep0, 1, (3U << 1) | ((uint32)EP_TYPE_CTRL << 3) | ((uint32)dev->mps0 << 16));
    uintptr rp = dev->ep_ring[DCI_EP0 - 1].phys;
    ctx_wr(ep0, 2, (uint32)((rp & ~0xFULL) | 1));
    ctx_wr(ep0, 3, (uint32)(rp >> 32));
    ctx_wr(ep0, 4, 8);
}

//build the configure endpoint context for a HID interrupt IN endpoint
static void build_config_hid_ctx(xhci_ctrl_t *c, xhci_device_t *dev,
                                  void *in_ctx, uint8 dci, uint8 hid_proto,
                                  uint16 mps, uint8 interval) {
    uint8 cs = c->ctx_size;

    uint32 *icc = ctx_block(in_ctx, cs, 0);
    ctx_wr(icc, 0, 0);
    //add slot (bit 0) + the endpoint DCI (bit dci)
    ctx_wr(icc, 1, (1U << 0) | (1U << dci));

    //update slot context - context entries must cover highest DCI
    uint32 *slot = ctx_block(in_ctx, cs, 1);
    ctx_wr(slot, 0, ((uint32)dev->speed << 20) | ((uint32)dci << 27));
    ctx_wr(slot, 1, (uint32)(dev->port + 1) << 16);
    ctx_wr(slot, 2, 0);
    ctx_wr(slot, 3, 0);

    //endpoint context for the HID interrupt IN endpoint (block = dci + 1)
    uint32 *ep = ctx_block(in_ctx, cs, dci + 1);

    //interval: for HS endpoints bInterval is 2^(n-1) microframes
    //or FS endpoints bInterval is in ms -> convert to 125µs microframes
    uint8 ep_interval;
    if (dev->speed == USB_SPEED_HS || dev->speed == USB_SPEED_SS) {
        ep_interval = (interval > 0) ? (interval - 1) : 0;
    } else {
        //FS: bInterval in ms -> microframes: ms * 8; interval field = log2(ms * 8)
        uint8 mf = interval * 8;
        ep_interval = 0;
        while (mf > 1) { mf >>= 1; ep_interval++; }
    }
    //clamp HID polling to sane minimums under high input rates:
    //keyboard >= 1ms, mouse >= 2ms
    if (hid_proto == USB_HID_PROTO_MOUSE && ep_interval < 4) {
        ep_interval = 4;
    } else if (hid_proto == USB_HID_PROTO_KEYBOARD && ep_interval < 3) {
        ep_interval = 3;
    }
    
    //NEC host erratum: rejects LS/FS interrupt endpoints with interval < 6
    if (xhci_has_quirk(c, XHCI_QUIRK_NEC_HOST) && ep_interval < 6) {
        ep_interval = 6;
    }
    
    if (ep_interval > 15) ep_interval = 15;

    ctx_wr(ep, 0, (uint32)ep_interval << 16);
    //CErr=3, EP type=interrupt IN, max burst=0, max packet size
    ctx_wr(ep, 1, (3U << 1) | ((uint32)EP_TYPE_INTR_IN << 3) | ((uint32)mps << 16));
    //TR dequeue pointer
    uintptr rp = dev->ep_ring[dci - 1].phys;
    ctx_wr(ep, 2, (uint32)((rp & ~0xFULL) | 1)); //DCS=1
    ctx_wr(ep, 3, (uint32)(rp >> 32));
    //DW4: Max ESIT Payload [31:16] | average TRB length [15:0]
    ctx_wr(ep, 4, ((uint32)mps << 16) | mps);
}

//event ring processing (called from IRQ handler and polling loops)

//event ring processing - split into three layers:
//xhci_drain_events; scan DMA ring memory only, no MMIO writes
//sfe to call millions of times in a tight loop
//xhci_ack_events: write ERDP/IMAN/USBSTS to acknowledge to HC
//called once after draining
//xhci_process_events: convenience wrapper (drain + ack) used from
//the IRQ handler path

static void xhci_drain_events(xhci_ctrl_t *c) {
    for (;;) {
        xhci_trb_t *trb = &c->evt_ring[c->evt_deq];

        //check cycle bit - if it doesn't match CCS the ring is empty
        if ((trb->control & TRB_C) != (c->evt_ccs ? TRB_C : 0))
            break;

        uint8  type = (trb->control >> TRB_TYPE_SHIFT) & 0x3F;
        uint8  cc   = (trb->status >> 24) & 0xFF;
        uint32 slot = (trb->control >> TRB_SLOT_SHIFT) & 0xFF;

        switch (type) {
        case TRB_TYPE_CMD_COMPLETE: {
            c->cmd_cc   = cc;
            //enable Slot returns the new slot ID in bits [31:24] of control
            if (cc == TRB_CC_SUCCESS)
                c->cmd_slot = slot;
            c->cmd_done = 1;
            break;
        }

        case TRB_TYPE_XFER_EVT: {
            //bits [31:24] = slot ID, bits [20:16] = endpoint DCI
            uint8  ep_dci   = (trb->control >> TRB_EP_SHIFT) & 0x1F;
            uint32 residual = trb->status & 0xFFFFFF;

            if (slot >= 1 && slot <= XHCI_MAX_SLOTS) {
                xhci_device_t *dev = &c->devices[slot];

                if (ep_dci == DCI_EP0) {
                    dev->ctrl_cc       = cc;
                    dev->ctrl_residual = residual;
                    dev->ctrl_done     = 1;
                } else if (dev->is_hid && ep_dci == dev->hid_ep_dci) {
                    if (dev->hid_recovering) break;
                    //this completion corresponds to the one armed interrupt TRB
                    dev->hid_intr_queued = false;
                    if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) {
                        uint32 actual = dev->hid_ep_mps - residual;
                        //defer HID processing to outside evt_lock so we
                        //don't hold it across kmalloc/channel/sched locks
                        if (c->hid_pending_count < HID_PENDING_MAX) {
                            hid_pending_t *hp = &c->hid_pending[c->hid_pending_count++];
                            hp->proto = dev->hid_proto;
                            hp->slot  = (uint8)slot;
                            hp->len   = (uint16)(actual > 64 ? 64 : actual);
                            memcpy(hp->data, dev->hid_buf, hp->len);
                        }
                    } else {
                        static bool hid_cc_reported[256];
                        if (!hid_cc_reported[cc]) {
                            hid_cc_reported[cc] = true;
                            printf("[xhci] HID transfer error cc=%u slot=%u dci=%u\n",
                                   cc, slot, ep_dci);
                        }
                        c->hid_recovery_needed = true;
                    }
                }
            }
            break;
        }

        case TRB_TYPE_HC_EVT:
            if (cc == TRB_CC_EVENT_RING_FULL || cc == TRB_CC_EVENT_LOST) {
                //defer recovery outside the drain loop
                //doing command traffic
                //inside event consumption can recurse badly under pressure
                c->hid_recovery_needed = true;
            }
            break;

        case TRB_TYPE_PORT_CHANGE: {
            //port ID is in DW1 bits [31:24] = param bits [63:56]
            uint8 port1 = (uint8)((trb->param >> 56) & 0xFF);
            if (port1 >= 1 && port1 <= c->max_ports) {
                if (!(op_read32(c, XHCI_PORTSC(port1 - 1)) & PORTSC_CCS)) {
                    c->disconnect_ports[port1] = true;
                }
                //acknowledge by clearing all W1C bits on that port
                uint32 portsc = op_read32(c, XHCI_PORTSC(port1 - 1));
                op_write32(c, XHCI_PORTSC(port1 - 1),
                           (portsc & ~PORTSC_W1C_MASK) | PORTSC_W1C_MASK);
            }
            break;
        }

        default:
            break;
        }

        evt_advance(c);
    }
}

static void xhci_ack_events(xhci_ctrl_t *c) {
    //clear interrupt-pending in the interrupter first
    uint32 iman = rt_read32(c, XHCI_RT_IMAN(0));
    rt_write32(c, XHCI_RT_IMAN(0), iman | IMAN_IP);
    
    //clear pending status bits (all W1C): event interrupt, port-change detect,
    //and host-system-error, leaving HSE/PCD latched can cause IRQ storms
    op_write32(c, XHCI_OP_USBSTS, USBSTS_EINT | USBSTS_PCD | USBSTS_HSE);

    //move the HC's dequeue pointer to where we've read up to and clear EHB
    evt_update_erdp(c);
}

static void xhci_process_events(xhci_ctrl_t *c) {
    //drain event ring and re-arm HID endpoints under lock
    //re-arming (xhci_queue_intr) must happen inside evt_lock so that no
    //concurrent drain on another CPU can observe hid_intr_queued in a
    //stale state, which would permanently kill the interrupt pipe on SMP
    irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
    
    //clear interrupt status before draining to avoid race conditions on 0.96 ocntrollers
    uint32 iman = rt_read32(c, XHCI_RT_IMAN(0));
    rt_write32(c, XHCI_RT_IMAN(0), iman | IMAN_IP);
    op_write32(c, XHCI_OP_USBSTS, USBSTS_EINT | USBSTS_PCD | USBSTS_HSE);

    c->hid_pending_count = 0;
    xhci_drain_events(c);
    
    //update ERDP and clear EHB after draining
    evt_update_erdp(c);
    uint32 n_pending = c->hid_pending_count;

    //re-arm HID interrupt endpoints while still holding evt_lock
    for (uint32 i = 0; i < n_pending; i++) {
        hid_pending_t *hp = &c->hid_pending[i];
        if (hp->slot >= 1 && hp->slot <= XHCI_MAX_SLOTS) {
            xhci_device_t *dev = &c->devices[hp->slot];
            if (dev->in_use && dev->is_hid)
                xhci_queue_intr(c, dev);
        }
    }
    spinlock_irq_release(&c->evt_lock, fl);

    xhci_process_disconnects(c);

    //deliver HID reports outside the lock (quite heavy work
    for (uint32 i = 0; i < n_pending; i++) {
        hid_pending_t *hp = &c->hid_pending[i];
        if (hp->slot >= 1 && hp->slot <= XHCI_MAX_SLOTS) {
            xhci_device_t *dev = &c->devices[hp->slot];
            if (dev->in_use && dev->is_hid)
                hid_report_received(hp->proto, hp->data, hp->len);
        }
    }
}

static void xhci_cleanup_device(xhci_ctrl_t *c, uint8 slot) {
    if (slot < 1 || slot > XHCI_MAX_SLOTS) return;

    xhci_device_t *dev = &c->devices[slot];

    //serialize the cleanup by ensuring only one waiter submits disable slot and frees resources
    irq_state_t fl_evt = spinlock_irq_acquire(&c->evt_lock);
    if (dev->disable_in_progress) {
        spinlock_irq_release(&c->evt_lock, fl_evt);
        return;
    }
    dev->disable_in_progress = true;
    spinlock_irq_release(&c->evt_lock, fl_evt);

    uint8 slot_id = dev->slot_id;

    if (dev->in_use && slot_id == slot) {
        (void)cmd_disable_slot_submit(c, slot_id);
    }

    if (dev->is_hid) {
        if (dev->hid_proto == USB_HID_PROTO_KEYBOARD) {
            hid_usb_keyboard_detached();
        } else if (dev->hid_proto == USB_HID_PROTO_MOUSE) {
            hid_usb_mouse_detached();
        }
    }

    if (dev->hid_buf_phys) {
        pmm_free(dev->hid_buf_phys, 1);
        dev->hid_buf_phys = NULL;
        dev->hid_buf = NULL;
    }

    for (uint32 i = 0; i < XHCI_MAX_EP; i++) {
        if (dev->ep_ring_ok[i]) {
            ring_free(&dev->ep_ring[i]);
            dev->ep_ring_ok[i] = false;
        }
    }

    if (dev->out_ctx_phys) {
        pmm_free(dev->out_ctx_phys, 1);
        dev->out_ctx_phys = NULL;
        dev->out_ctx = NULL;
    }

    if (c->dcbaa && slot <= c->max_slots) {
        c->dcbaa[slot] = 0;
    }

    memset(dev, 0, sizeof(*dev));
}

static void xhci_process_disconnects(xhci_ctrl_t *c) {
    for (uint32 port1 = 1; port1 <= c->max_ports && port1 < 256; port1++) {
        if (!c->disconnect_ports[port1]) continue;
        c->disconnect_ports[port1] = false;

        for (uint32 slot = 1; slot <= XHCI_MAX_SLOTS; slot++) {
            xhci_device_t *dev = &c->devices[slot];
            if (dev->slot_id == slot && dev->port == (port1 - 1) &&
                (dev->in_use || dev->out_ctx_phys || dev->hid_buf_phys)) {
                xhci_cleanup_device(c, (uint8)slot);
                break;
            }
        }
    }
}

static uint32 xhci_poll_pending_hid(xhci_ctrl_t *c) {
    irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
    c->hid_pending_count = 0;
    xhci_drain_events(c);
    uint32 n = c->hid_pending_count;
    for (uint32 j = 0; j < n; j++) {
        hid_pending_t *hp = &c->hid_pending[j];
        if (hp->slot >= 1 && hp->slot <= XHCI_MAX_SLOTS) {
            xhci_device_t *dev = &c->devices[hp->slot];
            if (dev->in_use && dev->is_hid)
                xhci_queue_intr(c, dev);
        }
    }
    spinlock_irq_release(&c->evt_lock, fl);

    xhci_process_disconnects(c);

    for (uint32 j = 0; j < n; j++)
        if (c->hid_pending[j].slot >= 1 && c->hid_pending[j].slot <= XHCI_MAX_SLOTS) {
            xhci_device_t *dev = &c->devices[c->hid_pending[j].slot];
            if (dev->in_use && dev->is_hid)
                hid_report_received(c->hid_pending[j].proto, c->hid_pending[j].data, c->hid_pending[j].len);
        }

    return n;
}

static void xhci_recover_hid_endpoints(xhci_ctrl_t *c) {
    for (uint32 s = 1; s <= XHCI_MAX_SLOTS; s++) {
        xhci_device_t *dev = &c->devices[s];
        if (!dev->in_use || !dev->is_hid) continue;

        //rebuild endpoint state if transfer events were lost or failed
        dev->hid_recovering = true;
        dev->hid_intr_queued = false;

        (void)cmd_reset_ep(c, dev->slot_id, dev->hid_ep_dci);
        ring_reinit(&dev->ep_ring[dev->hid_ep_dci - 1]);
        (void)cmd_set_tr_deq(c, dev->slot_id, dev->hid_ep_dci,
                             dev->ep_ring[dev->hid_ep_dci - 1].phys);

        dev->hid_recovering = false;
        xhci_queue_intr(c, dev);
    }
}

static int xhci_port_reset(xhci_ctrl_t *c, uint8 port_idx, uint8 speed) {
    uint32 portsc = op_read32(c, XHCI_PORTSC(port_idx));
    bool is_superspeed = (speed == USB_SPEED_SS || speed == USB_SPEED_SSP);
    bool is_v096 = (c->hci_ver < 0x0100);

    if (!(portsc & PORTSC_PP)) {
        op_write32(c, XHCI_PORTSC(port_idx), (portsc & ~PORTSC_W1C_MASK) | PORTSC_PP);
        sleep(20);
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
    }

    //on some 0.96 controllers setting PR might not clear automatically or 
    //might require specific timing
    op_write32(c, XHCI_PORTSC(port_idx), (portsc & ~PORTSC_W1C_MASK) | PORTSC_PR);

    //wait for reset to complete
    bool success = false;
    for (uint32 elapsed = 0; elapsed < 500; elapsed++) {
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
        
        //in 1.0+, we wait for PRC but in 0.96 we check if PR has cleared or PED is set
        if (is_v096) {
            if (!(portsc & PORTSC_PR) || (portsc & PORTSC_PED)) {
                success = true;
                break;
            }
        } else {
            if (portsc & PORTSC_PRC) {
                success = true;
                break;
            }
        }
        sleep(1);
    }

    if (!success) {
        printf("[xhci] port %u reset timeout (portsc=0x%08X)\n", port_idx, portsc);
    }

    //clear change bits
    portsc = op_read32(c, XHCI_PORTSC(port_idx));
    op_write32(c, XHCI_PORTSC(port_idx), (portsc & ~PORTSC_W1C_MASK) | (portsc & PORTSC_W1C_MASK));

    //settle time
    sleep(20);

    portsc = op_read32(c, XHCI_PORTSC(port_idx));
    uint32 pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;

    if (portsc & PORTSC_PED) return 0;
    if (is_superspeed && pls == PORTSC_PLS_U0) return 0;

    //if SS port is stuck in Polling (pls=7), it needs a Warm Reset on Renesas
    if (is_superspeed && pls == PORTSC_PLS_POLLING) {
        printf("[xhci] port %u: SS stuck in Polling, attempting Warm Reset\n", port_idx);
        op_write32(c, XHCI_PORTSC(port_idx), (portsc & ~PORTSC_W1C_MASK) | PORTSC_WPR);
        
        for (uint32 elapsed = 0; elapsed < 500; elapsed++) {
            portsc = op_read32(c, XHCI_PORTSC(port_idx));
            if (portsc & PORTSC_WRC) break;
            sleep(1);
        }
        
        //clear WRC
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
        op_write32(c, XHCI_PORTSC(port_idx), (portsc & ~PORTSC_W1C_MASK) | PORTSC_WRC);
        
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
        if (portsc & PORTSC_PED) return 0;
        pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (pls == PORTSC_PLS_U0) return 0;
    }

    return -1;
}

static bool xhci_port_polling_recovery(xhci_ctrl_t *c, uint8 port_idx) {
    if (!xhci_has_quirk(c, XHCI_QUIRK_PORT_POLLING_RECOVER)) return false;

    uint32 portsc = op_read32(c, XHCI_PORTSC(port_idx));
    uint8 speed = (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
    uint32 pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
    if (!(portsc & PORTSC_CCS)) return false;
    if (pls != PORTSC_PLS_POLLING && pls != PORTSC_PLS_COMP_MOD) return false;

    //first try one more normal reset, then some ports need to be fucking abused
    //this spec is bullshit it says we gotta do this ONCE AND cleanly 
    //EXACLY in xHCI spec § 4.3.1 - resetting a root hub port
    //but manufactuers making controlelrs don't follow it
    if (xhci_port_reset(c, port_idx, speed) == 0) {
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
        pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (portsc & PORTSC_PED) return true;
        if (speed == USB_SPEED_SS || speed == USB_SPEED_SSP) {
            if (pls == PORTSC_PLS_U0) return true;
        }
    }

    if (!xhci_has_quirk(c, XHCI_QUIRK_PORT_POLLING_WARM_RESET)) return false;

    printf("[xhci] port %u: polling recovery warm reset\n", port_idx);
    portsc = op_read32(c, XHCI_PORTSC(port_idx));
    op_write32(c, XHCI_PORTSC(port_idx),
               (portsc & ~PORTSC_W1C_MASK) | PORTSC_WPR);

    for (uint32 elapsed = 0; elapsed < 100; elapsed++) {
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
        pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (portsc & PORTSC_PED) return true;
        if (speed == USB_SPEED_SS || speed == USB_SPEED_SSP) {
            if (pls == PORTSC_PLS_U0) return true;
        }
        sleep(1);
    }

    return false;
}


//device enumeration
static void xhci_enumerate_device(xhci_ctrl_t *c, uint8 port_idx) {
    uint32 portsc = op_read32(c, XHCI_PORTSC(port_idx));
    uint8  speed  = (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;

    printf("[xhci] port %u: device connected (speed %u)\n", port_idx, speed);

    //enable slot
    int slot = cmd_enable_slot(c);
    if (slot < 1) {
        printf("[xhci] port %u: Enable Slot failed\n", port_idx);
        return;
    }

    xhci_device_t *dev = &c->devices[slot];
    memset(dev, 0, sizeof(*dev));
    dev->in_use  = true;
    dev->slot_id = (uint8)slot;
    dev->port    = port_idx;
    dev->speed   = speed;
    dev->mps0    = usb_default_mps0(speed);

    //allocate output device context
    void *out_phys = pmm_alloc(1);
    if (!out_phys) goto fail_slot;
    dev->out_ctx_phys = out_phys;
    dev->out_ctx      = P2V(out_phys);
    memset(dev->out_ctx, 0, PAGE_SIZE);
    c->dcbaa[slot]    = (uint64)(uintptr)out_phys;
    arch_wmb();

    //allocate EP0 transfer ring
    if (ring_alloc(&dev->ep_ring[DCI_EP0 - 1], XHCI_TR_RING_SIZE,
                   xhci_has_quirk(c, XHCI_QUIRK_LINK_TRB_CHAIN)) < 0) goto fail_ctx;
    dev->ep_ring_ok[DCI_EP0 - 1] = true;

    //address device (BSR=0 - sends SET_ADDRESS)
    uintptr in_ctx_phys = 0;
    void   *in_ctx = alloc_input_ctx(c, &in_ctx_phys);
    if (!in_ctx) goto fail_ep0;

    build_addr_device_ctx(c, dev, in_ctx);

    int cc = cmd_address_device(c, (uint8)slot, in_ctx_phys, false);
    if (cc != TRB_CC_SUCCESS) {
        printf("[xhci] port %u slot %u: Address Device failed (cc=%d)\n",
               port_idx, slot, cc);
        pmm_free((void *)in_ctx_phys, 1);
        goto fail_ep0;
    }

    //get first 8 bytes of device descriptor (to learn real mps0)
    void *desc_phys = pmm_alloc(1);
    if (!desc_phys) { pmm_free((void *)in_ctx_phys, 1); goto fail_ep0; }
    void *desc_virt = P2V(desc_phys);
    memset(desc_virt, 0, PAGE_SIZE);

    cc = xhci_get_descriptor(c, dev, USB_DESC_DEVICE, 0,
                             desc_virt, (uintptr)desc_phys, 8);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
        printf("[xhci] slot %u: GET_DESCRIPTOR(8) failed (cc=%d)\n", slot, cc);
        pmm_free(desc_phys, 1);
        pmm_free((void *)in_ctx_phys, 1);
        goto fail_ep0;
    }

    usb_device_desc_t *ddesc = (usb_device_desc_t *)desc_virt;
    uint16 real_mps0 = ddesc->bMaxPacketSize0;
    if (speed == USB_SPEED_SS || speed == USB_SPEED_SSP) real_mps0 = 512;
    if (real_mps0 == 0) real_mps0 = dev->mps0;

    //if mps0 changed issue evaluate context to inform the HC
    if (real_mps0 != dev->mps0) {
        dev->mps0 = real_mps0;
        memset(in_ctx, 0, PAGE_SIZE);
        build_eval_mps_ctx(c, dev, in_ctx);
        cmd_eval_ctx(c, (uint8)slot, in_ctx_phys);
    }

    //get full device descriptor
    memset(desc_virt, 0, PAGE_SIZE);
    cc = xhci_get_descriptor(c, dev, USB_DESC_DEVICE, 0,
                             desc_virt, (uintptr)desc_phys, 18);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
        printf("[xhci] slot %u: GET_DESCRIPTOR(18) failed\n", slot);
        pmm_free(desc_phys, 1);
        pmm_free((void *)in_ctx_phys, 1);
        goto fail_ep0;
    }

    uint8 dev_class    = ddesc->bDeviceClass;
    uint8 num_cfgs     = ddesc->bNumConfigurations;
    printf("[xhci] slot %u: VID=%04X PID=%04X class=%02X cfgs=%u\n",
           slot, ddesc->idVendor, ddesc->idProduct, dev_class, num_cfgs);

    //get configuration descriptor (first 9 bytes then full)
    memset(desc_virt, 0, PAGE_SIZE);
    cc = xhci_get_descriptor(c, dev, USB_DESC_CONFIG, 0,
                             desc_virt, (uintptr)desc_phys, 9);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
        printf("[xhci] slot %u: GET_DESCRIPTOR(config,9) failed\n", slot);
        pmm_free(desc_phys, 1);
        pmm_free((void *)in_ctx_phys, 1);
        goto fail_ep0;
    }

    usb_config_desc_t *cdesc = (usb_config_desc_t *)desc_virt;
    uint16 total_len = cdesc->wTotalLength;
    uint8  cfg_val   = cdesc->bConfigurationValue;
    if (total_len > PAGE_SIZE) total_len = PAGE_SIZE;

    memset(desc_virt, 0, PAGE_SIZE);
    cc = xhci_get_descriptor(c, dev, USB_DESC_CONFIG, 0,
                             desc_virt, (uintptr)desc_phys, total_len);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) {
        printf("[xhci] slot %u: GET_DESCRIPTOR(config,full) failed\n", slot);
        pmm_free(desc_phys, 1);
        pmm_free((void *)in_ctx_phys, 1);
        goto fail_ep0;
    }

    //walk descriptors looking for HID interface + interrupt endpoint
    bool       found_hid  = false;
    uint8      hid_proto  = 0;
    uint8      hid_iface  = 0;
    uint8      hid_iface_sub = 0;
    uint8      hid_iface_proto = 0;
    uint8      hid_ep_addr = 0;
    uint16     hid_ep_mps = 0;
    uint8      hid_ep_ivl = 10;
    uint16     hid_report_desc_len = 0;
    usb_hid_desc_t *hid_desc = NULL;

    uint8 *p   = (uint8 *)desc_virt;
    uint8 *end = p + total_len;
    uint8  cur_iface = 0xFF;
    uint8  cur_iface_class = 0;
    uint8  cur_iface_sub   = 0;
    uint8  cur_iface_proto = 0;

    while (p < end && p[0] >= 2) {
        uint8 dlen  = p[0];
        uint8 dtype = p[1];

        if (dtype == USB_DESC_INTERFACE) {
            usb_iface_desc_t *id = (usb_iface_desc_t *)p;
            cur_iface       = id->bInterfaceNumber;
            cur_iface_class = id->bInterfaceClass;
            cur_iface_sub   = id->bInterfaceSubClass;
            cur_iface_proto = id->bInterfaceProtocol;
        } else if (dtype == USB_DESC_HID) {
            hid_desc = (usb_hid_desc_t *)p;
            hid_report_desc_len = hid_desc->wDescriptorLength;
        } else if (dtype == USB_DESC_ENDPOINT && !found_hid) {
            usb_ep_desc_t *ed = (usb_ep_desc_t *)p;
            bool is_in   = USB_EP_IS_IN(ed->bEndpointAddress);
            uint8 xtype  = ed->bmAttributes & 0x03;

            if (cur_iface_class == USB_CLASS_HID &&
                is_in && xtype == USB_EP_XFER_INTR) {

                found_hid    = true;
                hid_proto    = cur_iface_proto; //1=kbd 2=mouse
                hid_iface    = cur_iface;
                hid_iface_sub = cur_iface_sub;
                hid_iface_proto = cur_iface_proto;
                hid_ep_addr  = ed->bEndpointAddress;
                hid_ep_mps   = ed->wMaxPacketSize;
                hid_ep_ivl   = ed->bInterval;
            }
        }

        p += dlen;
    }

    //set configuration
    cc = xhci_set_config(c, dev, cfg_val);
    if (cc != TRB_CC_SUCCESS) {
        printf("[xhci] slot %u: SET_CONFIG failed (cc=%d)\n", slot, cc);
        pmm_free(desc_phys, 1);
        pmm_free((void *)in_ctx_phys, 1);
        goto fail_ep0;
    }

    //if this is a HID interface, try to parse the report descriptor so we
    //can accept report-protocol devices instead of boot-only ones
    if (found_hid && hid_desc && hid_report_desc_len > 0) {
        void *report_phys = pmm_alloc(1);
        if (report_phys) {
            void *report_virt = P2V(report_phys);
            uint16 fetch_len = hid_report_desc_len;
            if (fetch_len > PAGE_SIZE) fetch_len = PAGE_SIZE;
            memset(report_virt, 0, PAGE_SIZE);

            cc = xhci_get_hid_report_descriptor(c, dev, hid_iface, report_virt, (uintptr)report_phys, fetch_len);
            if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PKT) {
                uint8 parsed_proto = 0;
                uint8 report_id = 0;
                uint16 report_len = 0;
                if (hid_parse_report_descriptor(report_virt, fetch_len, &parsed_proto, &report_id, &report_len)) {
                    hid_proto = parsed_proto;
                }
            }

            pmm_free(report_phys, 1);
        }
    }

    if (hid_proto != HID_PROTO_KEYBOARD && hid_proto != HID_PROTO_MOUSE) {
        if (found_hid && hid_iface_sub == USB_HID_SUBCLASS_BOOT &&
            (hid_iface_proto == USB_HID_PROTO_KEYBOARD || hid_iface_proto == USB_HID_PROTO_MOUSE)) {
            hid_proto = hid_iface_proto;
        } else {
            found_hid = false;
        }
    }

    //if HID configure endpoint and start polling
    if (found_hid &&
        (hid_proto == USB_HID_PROTO_KEYBOARD || hid_proto == USB_HID_PROTO_MOUSE)) {

        uint8 dci = EP_ADDR_TO_DCI(hid_ep_addr);
        if (dci < 1 || dci >= XHCI_MAX_EP + 1) {
            printf("[xhci] slot %u: bad HID EP DCI %u\n", slot, dci);
            goto skip_hid;
        }

        //allocate transfer ring for the HID interrupt endpoint
        if (ring_alloc(&dev->ep_ring[dci - 1], XHCI_TR_RING_SIZE,
                       xhci_has_quirk(c, XHCI_QUIRK_LINK_TRB_CHAIN)) < 0)
            goto skip_hid;
        dev->ep_ring_ok[dci - 1] = true;

        //configure endpoint command
        memset(in_ctx, 0, PAGE_SIZE);
        build_config_hid_ctx(c, dev, in_ctx, dci, hid_proto, hid_ep_mps, hid_ep_ivl);

        cc = cmd_configure_ep(c, (uint8)slot, in_ctx_phys);
        if (cc != TRB_CC_SUCCESS) {
            printf("[xhci] slot %u: Configure Endpoint failed (cc=%d)\n", slot, cc);
            goto skip_hid;
        }

        //boot-only interfaces can be told to use boot protocol, report-only
        //interfaces may stall those requests so only issue them when legal
        if (hid_iface_sub == USB_HID_SUBCLASS_BOOT) {
            xhci_hid_set_protocol(c, dev, hid_iface, 0);
            xhci_hid_set_idle(c, dev, hid_iface, 0);
        }

        //allocate DMA buffer for HID reports (one page)
        void *hid_phys = pmm_alloc(1);
        if (!hid_phys) goto skip_hid;
        dev->hid_buf_phys = hid_phys;
        dev->hid_buf      = P2V(hid_phys);
        memset(dev->hid_buf, 0, PAGE_SIZE);

        dev->is_hid      = true;
        dev->hid_proto   = hid_proto;
        dev->hid_ep_dci  = dci;
        dev->hid_ep_mps  = hid_ep_mps;
        dev->hid_interval = hid_ep_ivl;
        dev->hid_iface   = hid_iface;
        dev->hid_intr_queued = false;
        dev->hid_recovering = false;

        const char *proto_name = (hid_proto == USB_HID_PROTO_KEYBOARD) ? "keyboard" : "mouse";
        printf("[xhci] slot %u: HID %s (EP 0x%02X DCI %u mps %u ivl %u)\n",
               slot, proto_name, hid_ep_addr, dci, hid_ep_mps, hid_ep_ivl);

        //prime the interrupt pipe - queue the first transfer
        xhci_queue_intr(c, dev);
    }

skip_hid:
    pmm_free(desc_phys, 1);
    pmm_free((void *)in_ctx_phys, 1);
    return;

fail_ep0:
    xhci_cleanup_device(c, (uint8)slot);
    return;
fail_ctx:
    xhci_cleanup_device(c, (uint8)slot);
    return;
fail_slot:
    xhci_cleanup_device(c, (uint8)slot);
}

static void xhci_scan_ports(xhci_ctrl_t *c) {
    for (uint8 i = 0; i < c->max_ports; i++) {
        uint32 portsc = op_read32(c, XHCI_PORTSC(i));
        uint8  speed  = (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;

        if (!(portsc & PORTSC_CCS)) continue;   //nothing connected
        if (!(portsc & PORTSC_PP))  continue;   //port not powered

        //reset the port to bring it to the Enabled state
        if (xhci_port_reset(c, i, speed) < 0) continue;

        portsc = op_read32(c, XHCI_PORTSC(i));
        bool is_superspeed = (speed == USB_SPEED_SS || speed == USB_SPEED_SSP);
        uint32 pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (!(portsc & PORTSC_PED) && !(is_superspeed && pls == PORTSC_PLS_U0)) {
            if (xhci_port_polling_recovery(c, i)) {
                portsc = op_read32(c, XHCI_PORTSC(i));
                pls = (portsc & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
                if (portsc & PORTSC_PED || (is_superspeed && pls == PORTSC_PLS_U0)) {
                    xhci_enumerate_device(c, i);
                    continue;
                }
            }

            printf("[xhci] port %u: not enabled after reset (portsc=0x%08X ccs=%u ped=%u pp=%u pls=%u prc=%u)\n",
                   i, portsc,
                   !!(portsc & PORTSC_CCS),
                   !!(portsc & PORTSC_PED),
                   !!(portsc & PORTSC_PP),
                   pls,
                   !!(portsc & PORTSC_PRC));
            continue;
        }

        xhci_enumerate_device(c, i);
    }
}

//public IRQ entry point
void xhci_irq(void) {
    static bool hse_reported = false;
    static uint32 hid_recovery_count = 0;

    for (uint32 i = 0; i < g_ctrl_count; i++) {
        xhci_ctrl_t *c = g_ctrls[i];
        if (!c) continue;

        uint32 sts = op_read32(c, XHCI_OP_USBSTS);

        if ((sts & USBSTS_HSE) && !hse_reported) {
            printf("[xhci] host system error! USBSTS=0x%08X\n", sts);
            hse_reported = true;
        }

        //we are already on the xHCI MSI vector; always drain events
        //some controllers/VMs can present valid TRBs even when USBSTS bits are
        //momentarily clear by the time software reads them
        xhci_process_events(c);
        if (__atomic_exchange_n(&c->hid_recovery_needed, false, __ATOMIC_ACQ_REL)) {
            hid_recovery_count++;
            if (hid_recovery_count <= 16) {
                printf("[xhci] HID endpoint recovery pass %u\n", hid_recovery_count);
            }
            xhci_recover_hid_endpoints(c);
        }
    }
}

//controller initialisation
static void xhci_init_ctrl(pci_device_t *pci) {
    if (g_ctrl_count >= XHCI_MAX_CTRLS) return;

    xhci_ctrl_t *c = kzalloc(sizeof(xhci_ctrl_t));
    if (!c) return;

    c->pci = pci;
    xhci_init_quirks(c, pci);
    if (pci->vendor_id == 0x1033 && pci->device_id == 0x0194) {
        pci_disable_link_power_management(pci);
    }
    if (xhci_has_quirk(c, XHCI_QUIRK_RENESAS_FW_LOAD)) {
        (void)xhci_renesas_fw_load(pci);
    }
    pci_enable_mmio(pci);
    pci_enable_bus_master(pci);

    //map BAR0 (may be 64-bit)
    uint64 bar_phys = pci->bar[0].addr;
    uint64 bar_size = pci->bar[0].size;
    if (bar_size == 0) bar_size = 0x10000;

    uint32 pages = (uint32)((bar_size + PAGE_SIZE - 1) / PAGE_SIZE);
    c->cap_base  = (void *)P2V(bar_phys);
    vmm_kernel_map((uintptr)c->cap_base, bar_phys, pages,
                   MMU_FLAG_WRITE | MMU_FLAG_NOCACHE);

    printf("[xhci] BAR0 phys=0x%llX virt=%p size=%llu\n",
           (unsigned long long)bar_phys, c->cap_base,
           (unsigned long long)bar_size);

    //read capability registers
    uint32 cap0      = cap_read32(c, 0);
    uint8  caplength  = (uint8)(cap0 & 0xFF);
    c->hci_ver        = (uint16)(cap0 >> 16);

    uint32 hcsparams1 = cap_read32(c, XHCI_CAP_HCSPARAMS1);
    uint32 hccparams1 = cap_read32(c, XHCI_CAP_HCCPARAMS1);
    uint32 dboff      = cap_read32(c, XHCI_CAP_DBOFF);
    uint32 rtsoff     = cap_read32(c, XHCI_CAP_RTSOFF);

    c->op_base  = (uint8 *)c->cap_base + caplength;
    c->rt_base  = (uint8 *)c->cap_base + rtsoff;
    c->db_base  = (uint32 *)((uint8 *)c->cap_base + dboff);

    c->ctx_size  = (hccparams1 & HCCPARAMS1_CSZ) ? 64 : 32;
    c->max_ports = (uint8)HCSPARAMS1_MAX_PORTS(hcsparams1);
    c->max_slots = (uint8)HCSPARAMS1_MAX_SLOTS(hcsparams1);
    if (c->max_slots > XHCI_MAX_SLOTS) c->max_slots = XHCI_MAX_SLOTS;

    printf("[xhci] version 0x%04X ctx_size=%u max_ports=%u max_slots=%u\n",
           c->hci_ver, c->ctx_size, c->max_ports, c->max_slots);

    xhci_claim_bios_ownership(c, hccparams1);

    //wait for controller not ready to clear (up to 1 s)
    for (uint32 elapsed = 0; elapsed < 1000; elapsed++) {
        if (!(op_read32(c, XHCI_OP_USBSTS) & USBSTS_CNR)) break;
        sleep(1);
    }

    //stop controller if running
    uint32 cmd = op_read32(c, XHCI_OP_USBCMD);
    if (cmd & USBCMD_RUN) {
        op_write32(c, XHCI_OP_USBCMD, cmd & ~USBCMD_RUN);
        for (uint32 elapsed = 0; elapsed < 1000; elapsed++) {
            if (op_read32(c, XHCI_OP_USBSTS) & USBSTS_HCH) break;
            sleep(1);
        }
    }

    //reset the controller
    op_write32(c, XHCI_OP_USBCMD, USBCMD_HCRST);
    for (uint32 elapsed = 0; elapsed < 1000; elapsed++) {
        if (!(op_read32(c, XHCI_OP_USBCMD) & USBCMD_HCRST) &&
            !(op_read32(c, XHCI_OP_USBSTS) & USBSTS_CNR))
            break;
        sleep(1);
    }
    printf("[xhci] reset complete\n");

    //DCBAA
    void *dcbaa_phys = pmm_alloc(1);
    if (!dcbaa_phys) { kfree(c); return; }
    c->dcbaa      = (uint64 *)P2V(dcbaa_phys);
    c->dcbaa_phys = (uintptr)dcbaa_phys;
    memset(c->dcbaa, 0, PAGE_SIZE);
    op_write64(c, XHCI_OP_DCBAAP, c->dcbaa_phys);

    //command ring
    spinlock_irq_init(&c->cmd_lock);
    spinlock_irq_init(&c->evt_lock);
    if (ring_alloc(&c->cmd_ring, XHCI_CMD_RING_SIZE,
                   xhci_has_quirk(c, XHCI_QUIRK_LINK_TRB_CHAIN)) < 0) {
        pmm_free(dcbaa_phys, 1); kfree(c); return;
    }
    //CRCR: physical base of command ring | RCS (ring cycle state = initial PCS = 1)
    op_write64(c, XHCI_OP_CRCR, c->cmd_ring.phys | CRCR_RCS);

    //event ring
    uint32 evt_bytes = XHCI_EVT_RING_SIZE * (uint32)sizeof(xhci_trb_t);
    uint32 evt_pages = (evt_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *evt_phys = pmm_alloc(evt_pages);
    if (!evt_phys) { kfree(c); return; }
    c->evt_ring      = (xhci_trb_t *)P2V(evt_phys);
    c->evt_ring_phys = (uintptr)evt_phys;
    memset(c->evt_ring, 0, evt_pages * PAGE_SIZE);
    c->evt_deq = 0;
    c->evt_ccs = 1;

    //event ring segment table (ERST) - single segment
    void *erst_phys = pmm_alloc(1);
    if (!erst_phys) { kfree(c); return; }
    c->erst      = (xhci_erst_entry_t *)P2V(erst_phys);
    c->erst_phys = (uintptr)erst_phys;
    memset(c->erst, 0, PAGE_SIZE);
    c->erst[0].base = c->evt_ring_phys;
    c->erst[0].size = XHCI_EVT_RING_SIZE;
    arch_wmb();

    //program interrupter 0
    rt_write32(c, XHCI_RT_ERSTSZ(0), 1);
    rt_write64(c, XHCI_RT_ERSTBA(0), c->erst_phys);
    rt_write64(c, XHCI_RT_ERDP(0),   c->evt_ring_phys | ERDP_EHB);
    //keep moderation small under mixed keyboard+mouse spam so completions
    //are drained before event-ring pressure builds.
    //IMOD units are 250ns, so 1000 == 250us
    rt_write32(c, XHCI_RT_IMOD(0), 1000);

    //enable interrupter 0 (IE bit)
    rt_write32(c, XHCI_RT_IMAN(0), IMAN_IE | IMAN_IP);

    //max slots
    op_write32(c, XHCI_OP_CONFIG, CONFIG_MAX_SLOTS_EN(c->max_slots));

    //interrupts
    if (xhci_enable_interrupts(c) < 0)
        printf("[xhci] MSI/MSI-X unavailable - interrupts will not work\n");

    //start controller
    op_write32(c, XHCI_OP_USBCMD, USBCMD_RUN | USBCMD_INTE | USBCMD_HSEE);
    for (uint32 elapsed = 0; elapsed < 1000; elapsed++) {
        if (!(op_read32(c, XHCI_OP_USBSTS) & USBSTS_HCH)) break;
        sleep(1);
    }

    if (op_read32(c, XHCI_OP_USBSTS) & USBSTS_HCH) {
        printf("[xhci] ERR: controller failed to start\n");
        kfree(c); return;
    }

    printf("[xhci] controller running\n");
    g_ctrl_bar[g_ctrl_count] = bar_phys;   //record BAR0 for dedup check
    g_ctrls[g_ctrl_count++]  = c;

    if (!xhci_has_quirk(c, XHCI_QUIRK_RENESAS_FW_LOAD)) {
        xhci_nec_get_fw(c);
    }

    //settling delay, gives the cntroler time to complete port detection
    sleep(100);

    //scan ports and enumerate attached devices
    xhci_scan_ports(c);
}

static void xhci_init_quirks(xhci_ctrl_t *c, pci_device_t *pci) {
    if (!c || !pci) return;

    //renesas/NEC 1033:0194 can leave ports stuck in polling after reset
    if (pci->vendor_id == 0x1033 && pci->device_id == 0x0194) {
        c->quirks |= XHCI_QUIRK_PORT_POLLING_RECOVER;
        c->quirks |= XHCI_QUIRK_PORT_POLLING_WARM_RESET;
        c->quirks |= XHCI_QUIRK_FORCE_BIOS_HANDOFF;
        c->quirks |= XHCI_QUIRK_NEC_HOST;
        c->quirks |= XHCI_QUIRK_RENESAS_FW_LOAD;
    }
}

//driver entry point
void xhci_init(void) {
    pci_device_t *pdev = pci_get_devices();
    while (pdev) {
        //USB xHCI: class 0x0C, subclass 0x03, prog-if 0x30
        if (pdev->class_code == 0x0C &&
            pdev->subclass   == 0x03 &&
            pdev->prog_if    == 0x30) {

            //skip if we already own a controller at this BAR0 address
            //the PCI device list can contain duplicate entries for the same
            //physical device (pre-existing PCI scanner quirk with multifunction
            //devices), which would cause a double reset + double init
            uint64 bar0 = pdev->bar[0].addr;
            bool already_init = false;
            for (uint32 i = 0; i < g_ctrl_count; i++) {
                if (g_ctrl_bar[i] == bar0) { already_init = true; break; }
            }
            if (already_init) {
                printf("[xhci] skipping duplicate controller at BAR0=0x%llX\n",
                       (unsigned long long)bar0);
                pdev = pdev->next;
                continue;
            }

            printf("[xhci] found controller %04X:%04X at %02X:%02X.%X\n",
                   pdev->vendor_id, pdev->device_id,
                   pdev->bus, pdev->dev, pdev->func);
            xhci_init_ctrl(pdev);
        }
        pdev = pdev->next;
    }
}

DECLARE_DRIVER(xhci_init, INIT_LEVEL_DEVICE);
