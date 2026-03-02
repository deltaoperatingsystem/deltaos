#include <drivers/xhci.h>
#include <drivers/usb.h>
#include <drivers/hid.h>
#include <drivers/pci.h>
#include <drivers/init.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <arch/percpu.h>
#include <arch/amd64/int/apic.h>
#include <lib/io.h>
#include <lib/string.h>
#include <proc/thread.h>
#include <proc/wait.h>

//up to 4 xHCI controllers
#define XHCI_MAX_CTRLS 4
static xhci_ctrl_t *g_ctrls[XHCI_MAX_CTRLS];
static uint32       g_ctrl_count = 0;
//track which BAR0 physical addresses are already initialised (dedup guard)
static uint64       g_ctrl_bar[XHCI_MAX_CTRLS];

//forward declarations
static void xhci_drain_events(xhci_ctrl_t *c);
static void xhci_ack_events(xhci_ctrl_t *c);
static void xhci_process_events(xhci_ctrl_t *c);
static void xhci_recover_hid_endpoints(xhci_ctrl_t *c);
static void xhci_queue_intr(xhci_ctrl_t *c, xhci_device_t *dev);

//register accessors
static inline uint8 cap_read8(xhci_ctrl_t *c, uint32 off) {
    return *(volatile uint8 *)((uint8 *)c->cap_base + off);
}
static inline uint32 cap_read32(xhci_ctrl_t *c, uint32 off) {
    return *(volatile uint32 *)((uint8 *)c->cap_base + off);
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
}

static int ring_alloc(xhci_ring_t *ring, uint32 size) {
    uint32 bytes = size * (uint32)sizeof(xhci_trb_t);
    uint32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    void *phys = pmm_alloc(pages);
    if (!phys) return -1;

    ring->trbs = (xhci_trb_t *)P2V(phys);
    ring->phys = (uintptr)phys;
    ring->size = size;
    ring_reinit(ring);

    return 0;
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

    //route MSI-X to BSP (CPU0) LAPIC ID
    //this keeps runtime IRQ affinity correct even if init ever runs off-BSP
    uint8 dest_apic_id = (uint8)apic_get_id();
    percpu_t *bsp = percpu_get_by_index(0);
    if (bsp) dest_apic_id = (uint8)bsp->apic_id;

    c->msix_table[0].msg_addr_lo  = 0xFEE00000 | ((uint32)dest_apic_id << 12);
    c->msix_table[0].msg_addr_hi  = 0;
    c->msix_table[0].msg_data     = XHCI_MSI_VECTOR;
    c->msix_table[0].vector_ctrl  = 0; //unmask

    //enable MSI-X in the capability (bit 15 of message control)
    uint16 msgctl = pci_config_read(pci->bus, pci->dev, pci->func, c->msix_cap + 2, 2);
    pci_config_write(pci->bus, pci->dev, pci->func, c->msix_cap + 2, 2, msgctl | (1 << 15));

    printf("[xhci] MSI-X enabled (vector 0x%02X, dest APIC ID %u)\n",
           XHCI_MSI_VECTOR, dest_apic_id);
    return 0;
}

//wait for a command completion event
static int xhci_wait_cmd(xhci_ctrl_t *c) {

    //fast spin - most commands complete within a few microseconds
    for (int i = 0; i < 200000; i++) {
        if (c->cmd_done) goto done;
        arch_pause();
    }

    //slower poll: drain the event ring (DMA reads only, no MMIO) until
    //done or timeout
    for (int i = 0; i < 2000000; i++) {
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
        for (uint32 j = 0; j < n; j++)
            hid_report_received(c->hid_pending[j].proto, c->hid_pending[j].data, c->hid_pending[j].len);
        if (c->cmd_done) goto done;
        arch_pause();
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

    for (int i = 0; i < 200000; i++) {
        if (dev->ctrl_done) goto done;
        arch_pause();
    }

    for (int i = 0; i < 2000000; i++) {
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
        for (uint32 j = 0; j < n; j++)
            hid_report_received(c->hid_pending[j].proto, c->hid_pending[j].data, c->hid_pending[j].len);
        if (dev->ctrl_done) goto done;
        arch_pause();
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

static int cmd_address_device(xhci_ctrl_t *c, uint8 slot,
                               uintptr in_ctx_phys, bool bsr) {
    irq_state_t fl = spinlock_irq_acquire(&c->cmd_lock);
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
    //DW0: speed [23:20], Context Entries [31:28] = 1
    ctx_wr(slot, 0, ((uint32)dev->speed << 20) | (1U << 28));
    //DW1: Root Hub Port Number [23:16] (1-based)
    ctx_wr(slot, 1, (uint32)(dev->port + 1) << 16);
    ctx_wr(slot, 2, 0);
    ctx_wr(slot, 3, 0);

    //block 2 = EP0 context (DCI 1)
    uint32 *ep0 = ctx_block(in_ctx, cs, 2);
    //DW0: Interval = 0
    ctx_wr(ep0, 0, 0);
    //DW1: CErr=3, EP Type=4 (control bidir), max  packet size
    ctx_wr(ep0, 1, (3U << 0) | ((uint32)EP_TYPE_CTRL << 3) | ((uint32)dev->mps0 << 16));
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
    ctx_wr(ep0, 1, (3U << 0) | ((uint32)EP_TYPE_CTRL << 3) | ((uint32)dev->mps0 << 16));
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
    ctx_wr(slot, 0, ((uint32)dev->speed << 20) | ((uint32)dci << 28));
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
    if (ep_interval > 15) ep_interval = 15;

    ctx_wr(ep, 0, (uint32)ep_interval << 16);
    //CErr=3, EP type=interrupt IN, max burst=0, max packet size
    ctx_wr(ep, 1, (3U << 0) | ((uint32)EP_TYPE_INTR_IN << 3) | ((uint32)mps << 16));
    //TR dequeue pointer
    uintptr rp = dev->ep_ring[dci - 1].phys;
    ctx_wr(ep, 2, (uint32)((rp & ~0xFULL) | 1)); //DCS=1
    ctx_wr(ep, 3, (uint32)(rp >> 32));
    //average TRB length = mps for interrupt
    ctx_wr(ep, 4, mps);
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
    //move the HC's dequeue pointer to where we've read up to
    evt_update_erdp(c);
    //clear interrupt-pending in the interrupter
    uint32 iman = rt_read32(c, XHCI_RT_IMAN(0));
    rt_write32(c, XHCI_RT_IMAN(0), iman | IMAN_IP);
    //clear pending status bits (all W1C): event interrupt, port-change detect,
    //and host-system-error, leaving HSE/PCD latched can cause IRQ storms
    op_write32(c, XHCI_OP_USBSTS, USBSTS_EINT | USBSTS_PCD | USBSTS_HSE);
}

static void xhci_process_events(xhci_ctrl_t *c) {
    //drain event ring and re-arm HID endpoints under lock
    //re-arming (xhci_queue_intr) must happen inside evt_lock so that no
    //concurrent drain on another CPU can observe hid_intr_queued in a
    //stale state, which would permanently kill the interrupt pipe on SMP
    irq_state_t fl = spinlock_irq_acquire(&c->evt_lock);
    c->hid_pending_count = 0;
    xhci_drain_events(c);
    xhci_ack_events(c);
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

    //deliver HID reports outside the lock (quite heavy work
    for (uint32 i = 0; i < n_pending; i++) {
        hid_pending_t *hp = &c->hid_pending[i];
        hid_report_received(hp->proto, hp->data, hp->len);
    }
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

static int xhci_port_reset(xhci_ctrl_t *c, uint8 port_idx) {
    uint32 portsc = op_read32(c, XHCI_PORTSC(port_idx));

    if (!(portsc & PORTSC_PP)) {
        //power up the port first
        op_write32(c, XHCI_PORTSC(port_idx),
                   (portsc & ~PORTSC_W1C_MASK) | PORTSC_PP);
        for (int i = 0; i < 100000; i++) arch_pause();
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
    }

    //assert reset - preserve PP, clear W1C, set PR
    op_write32(c, XHCI_PORTSC(port_idx),
               (portsc & ~PORTSC_W1C_MASK) | PORTSC_PR);

    //wait for PRC (port reset change) with timeout ~500 ms
    for (int i = 0; i < 5000000; i++) {
        portsc = op_read32(c, XHCI_PORTSC(port_idx));
        if (portsc & PORTSC_PRC) break;
        arch_pause();
    }

    if (!(op_read32(c, XHCI_PORTSC(port_idx)) & PORTSC_PRC)) {
        printf("[xhci] port %u reset timeout\n", port_idx);
        return -1;
    }

    //clear PRC
    portsc = op_read32(c, XHCI_PORTSC(port_idx));
    op_write32(c, XHCI_PORTSC(port_idx),
               (portsc & ~PORTSC_W1C_MASK) | PORTSC_PRC);

    //short settling delay
    for (int i = 0; i < 500000; i++) arch_pause();

    return 0;
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
    if (ring_alloc(&dev->ep_ring[DCI_EP0 - 1], XHCI_TR_RING_SIZE) < 0) goto fail_ctx;
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
    uint8      hid_ep_addr = 0;
    uint16     hid_ep_mps = 0;
    uint8      hid_ep_ivl = 10;

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
        } else if (dtype == USB_DESC_ENDPOINT && !found_hid) {
            usb_ep_desc_t *ed = (usb_ep_desc_t *)p;
            bool is_in   = USB_EP_IS_IN(ed->bEndpointAddress);
            uint8 xtype  = ed->bmAttributes & 0x03;

            if (cur_iface_class == USB_CLASS_HID &&
                cur_iface_sub   == USB_HID_SUBCLASS_BOOT &&
                is_in && xtype == USB_EP_XFER_INTR) {

                //boot-protocol HID device with an interrupt IN endpoint
                found_hid    = true;
                hid_proto    = cur_iface_proto; //1=kbd 2=mouse
                hid_iface    = cur_iface;
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

    //if HID configure endpoint and start polling
    if (found_hid &&
        (hid_proto == USB_HID_PROTO_KEYBOARD || hid_proto == USB_HID_PROTO_MOUSE)) {

        uint8 dci = EP_ADDR_TO_DCI(hid_ep_addr);
        if (dci < 1 || dci >= XHCI_MAX_EP + 1) {
            printf("[xhci] slot %u: bad HID EP DCI %u\n", slot, dci);
            goto skip_hid;
        }

        //allocate transfer ring for the HID interrupt endpoint
        if (ring_alloc(&dev->ep_ring[dci - 1], XHCI_TR_RING_SIZE) < 0)
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

        //tell device to use boot protocol (protocol 0)
        xhci_hid_set_protocol(c, dev, hid_iface, 0);

        //set idle rate to 0 (device only reports on change)
        xhci_hid_set_idle(c, dev, hid_iface, 0);

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
    //TODO: free EP0 ring pages
    return;
fail_ctx:
    pmm_free(out_phys, 1);
    dev->out_ctx_phys = NULL;
    return;
fail_slot:
    dev->in_use = false;
}

static void xhci_scan_ports(xhci_ctrl_t *c) {
    for (uint8 i = 0; i < c->max_ports; i++) {
        uint32 portsc = op_read32(c, XHCI_PORTSC(i));

        if (!(portsc & PORTSC_CCS)) continue;   //nothing connected
        if (!(portsc & PORTSC_PP))  continue;   //port not powered

        //reset the port to bring it to the Enabled state
        if (xhci_port_reset(c, i) < 0) continue;

        portsc = op_read32(c, XHCI_PORTSC(i));
        if (!(portsc & PORTSC_PED)) {
            printf("[xhci] port %u: not enabled after reset (portsc=0x%08X)\n", i, portsc);
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
    uint8  caplength  = cap_read8(c,  XHCI_CAP_CAPLENGTH);
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

    printf("[xhci] ctx_size=%u max_ports=%u max_slots=%u\n",
           c->ctx_size, c->max_ports, c->max_slots);

    //wait for controller not ready to clear (up to 1 s)
    for (int i = 0; i < 10000000; i++) {
        if (!(op_read32(c, XHCI_OP_USBSTS) & USBSTS_CNR)) break;
        arch_pause();
    }

    //stop controller if running
    uint32 cmd = op_read32(c, XHCI_OP_USBCMD);
    if (cmd & USBCMD_RUN) {
        op_write32(c, XHCI_OP_USBCMD, cmd & ~USBCMD_RUN);
        for (int i = 0; i < 2000000; i++) {
            if (op_read32(c, XHCI_OP_USBSTS) & USBSTS_HCH) break;
            arch_pause();
        }
    }

    //reset the controller
    op_write32(c, XHCI_OP_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 10000000; i++) {
        if (!(op_read32(c, XHCI_OP_USBCMD) & USBCMD_HCRST) &&
            !(op_read32(c, XHCI_OP_USBSTS) & USBSTS_CNR))
            break;
        arch_pause();
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
    if (ring_alloc(&c->cmd_ring, XHCI_CMD_RING_SIZE) < 0) {
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

    //MSI-X
    if (xhci_enable_msix(c) < 0)
        printf("[xhci] MSI-X unavailable - interrupts will not work\n");

    //start controller
    op_write32(c, XHCI_OP_USBCMD, USBCMD_RUN | USBCMD_INTE | USBCMD_HSEE);
    for (int i = 0; i < 2000000; i++) {
        if (!(op_read32(c, XHCI_OP_USBSTS) & USBSTS_HCH)) break;
        arch_pause();
    }

    if (op_read32(c, XHCI_OP_USBSTS) & USBSTS_HCH) {
        printf("[xhci] ERR: controller failed to start\n");
        kfree(c); return;
    }

    printf("[xhci] controller running\n");
    g_ctrl_bar[g_ctrl_count] = bar_phys;   //record BAR0 for dedup check
    g_ctrls[g_ctrl_count++]  = c;

    //longer settling delay: gives QEMU time to complete port detection
    //before we scan  each arch_pause on KVM is ~5–10 ns; 10 M ≈ 50–100 ms
    for (int i = 0; i < 10000000; i++) arch_pause();

    //scan ports and enumerate attached devices
    xhci_scan_ports(c);
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
