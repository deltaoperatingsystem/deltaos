#ifndef DRIVERS_XHCI_H
#define DRIVERS_XHCI_H

#include <arch/types.h>
#include <drivers/pci.h>
#include <proc/wait.h>
#include <lib/spinlock.h>

//capability register offsets (from BAR0 base)
#define XHCI_CAP_CAPLENGTH      0x00    //1 byte: length of capability regs
#define XHCI_CAP_HCIVERSION     0x02    //2 bytes: xHCI spec version
#define XHCI_CAP_HCSPARAMS1     0x04    //max slots, interrupts, ports
#define XHCI_CAP_HCSPARAMS2     0x08    //ERST max, scratchpad bufs
#define XHCI_CAP_HCSPARAMS3     0x0C    //exit latencies
#define XHCI_CAP_HCCPARAMS1     0x10    //64-bit, context size, etc.
#define XHCI_CAP_DBOFF          0x14    //doorbell array offset from base
#define XHCI_CAP_RTSOFF         0x18    //runtime register space offset

//HCSPARAMS1 fields
#define HCSPARAMS1_MAX_SLOTS(x) ((x) & 0xFF)
#define HCSPARAMS1_MAX_INTRS(x) (((x) >> 8) & 0x7FF)
#define HCSPARAMS1_MAX_PORTS(x) (((x) >> 24) & 0xFF)

//HCCPARAMS1 bits
#define HCCPARAMS1_AC64         (1 << 0)    //64-bit addressing supported
#define HCCPARAMS1_CSZ          (1 << 2)    //context size: 0=32B, 1=64B
#define HCCPARAMS1_XECP(x)      (((x) >> 16) & 0xFFFF)  //xHCI extended cap ptr

//extended capability IDs
#define XHCI_EXT_CAPS_LEGACY    1
#define XHCI_EXT_CAPS_PROTOCOL   2

//USB legacy support capability bits
#define XHCI_HC_BIOS_OWNED      (1 << 16)
#define XHCI_HC_OS_OWNED        (1 << 24)
#define XHCI_LEGACY_SUPPORT_OFFSET 0x00
#define XHCI_LEGACY_CONTROL_OFFSET 0x04
#define XHCI_LEGACY_DISABLE_SMI ((0x7 << 1) + (0xff << 5) + (0x7 << 17))
#define XHCI_LEGACY_SMI_EVENTS  (0x7 << 29)

//operational register offsets (from op_base = BAR0 + CAPLENGTH)
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR            0x18    //64-bit: command ring control
#define XHCI_OP_DCBAAP          0x30    //64-bit: device context base addr array
#define XHCI_OP_CONFIG          0x38

//USBCMD bits
#define USBCMD_RUN              (1 << 0)    //run/stop
#define USBCMD_HCRST            (1 << 1)    //host controller reset
#define USBCMD_INTE             (1 << 2)    //interrupter enable
#define USBCMD_HSEE             (1 << 3)    //host system error enable
#define USBCMD_EU3S             (1 << 11)   //enable U3 MFINDEX stop

//USBSTS bits
#define USBSTS_HCH              (1 << 0)    //HC halted
#define USBSTS_HSE              (1 << 2)    //host system error
#define USBSTS_EINT             (1 << 3)    //event interrupt
#define USBSTS_PCD              (1 << 4)    //port change detect
#define USBSTS_CNR              (1 << 11)   //controller not ready
#define USBSTS_HCE              (1 << 12)   //HC error

//CRCR bits
#define CRCR_RCS                (1 << 0)    //ring cycle state
#define CRCR_CS                 (1 << 1)    //command stop
#define CRCR_CA                 (1 << 2)    //command abort
#define CRCR_CRR                (1 << 3)    //command ring running

//CONFIG register
#define CONFIG_MAX_SLOTS_EN(n)  ((n) & 0xFF)

//port status/control offsets (per-port at OPBASE + 0x400 + port*0x10)
#define XHCI_PORT_BASE          0x400
#define XHCI_PORT_STRIDE        0x10
#define XHCI_PORTSC(n)          (XHCI_PORT_BASE + (n) * XHCI_PORT_STRIDE + 0x00)
#define XHCI_PORTPMSC(n)        (XHCI_PORT_BASE + (n) * XHCI_PORT_STRIDE + 0x04)
#define XHCI_PORTLI(n)          (XHCI_PORT_BASE + (n) * XHCI_PORT_STRIDE + 0x08)

//PORTSC bits
#define PORTSC_CCS              (1 << 0)    //current connect status
#define PORTSC_PED              (1 << 1)    //port enabled
#define PORTSC_OCA              (1 << 3)    //over-current active
#define PORTSC_PR               (1 << 4)    //port reset (write 1 to reset)
#define PORTSC_PLS_MASK         (0xF << 5)
#define PORTSC_PLS_SHIFT        5
#define PORTSC_PLS_U0           0       //link active / enabled
#define PORTSC_PLS_POLLING      7       //SuperSpeed polling state
#define PORTSC_PLS_COMP_MOD     10      //SuperSpeed compliance mode
#define PORTSC_PP               (1 << 9)    //port power
#define PORTSC_SPEED_MASK       (0xF << 10)
#define PORTSC_SPEED_SHIFT      10
#define PORTSC_CSC              (1 << 17)   //connect status change  (W1C)
#define PORTSC_PEC              (1 << 18)   //port enabled change    (W1C)
#define PORTSC_WRC              (1 << 19)   //warm reset change      (W1C)
#define PORTSC_OCC              (1 << 20)   //over-current change    (W1C)
#define PORTSC_PRC              (1 << 21)   //port reset change      (W1C)
#define PORTSC_PLC              (1 << 22)   //port link state change (W1C)
#define PORTSC_CEC              (1 << 23)   //port config error      (W1C)
#define PORTSC_WPR              (1U << 31)   //warm port reset

//all write-1-to-clear bits in PORTSC (must not be accidentally asserted on RMW)
#define PORTSC_W1C_MASK         (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
                                 PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

//runtime register offsets (from rt_base = BAR0 + RTSOFF)
#define XHCI_RT_MFINDEX         0x00

//interrupter register set for interrupter N (one per MSI-X vector)
#define XHCI_IR_BASE(n)         (0x20 + (n) * 0x20)
#define XHCI_RT_IMAN(n)         (XHCI_IR_BASE(n) + 0x00)   //interrupt management
#define XHCI_RT_IMOD(n)         (XHCI_IR_BASE(n) + 0x04)   //interrupt moderation
#define XHCI_RT_ERSTSZ(n)       (XHCI_IR_BASE(n) + 0x08)   //ERST size
#define XHCI_RT_ERSTBA(n)       (XHCI_IR_BASE(n) + 0x10)   //ERST base addr (64-bit)
#define XHCI_RT_ERDP(n)         (XHCI_IR_BASE(n) + 0x18)   //event ring dequeue ptr (64-bit)

//IMAN bits
#define IMAN_IP                 (1 << 0)    //interrupt pending (W1C)
#define IMAN_IE                 (1 << 1)    //interrupt enable

//ERDP bits
#define ERDP_EHB                (1 << 3)    //event handler busy (W1C to acknowledge)
#define ERDP_DESI_MASK          (0x7)       //dequeue event ring segment index

//TRB types (bits [15:10] of control dword)

//transfer TRBs
#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_SETUP          2
#define TRB_TYPE_DATA           3
#define TRB_TYPE_STATUS         4
#define TRB_TYPE_ISOCH          5
#define TRB_TYPE_LINK           6
#define TRB_TYPE_EVENT_DATA     7
#define TRB_TYPE_NOOP_XFER      8
//command TRBs
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_DISABLE_SLOT   10
#define TRB_TYPE_ADDR_DEVICE    11
#define TRB_TYPE_CONFIG_EP      12
#define TRB_TYPE_EVAL_CTX       13
#define TRB_TYPE_RESET_EP       14
#define TRB_TYPE_STOP_EP        15
#define TRB_TYPE_SET_TRDEQ      16
#define TRB_TYPE_RESET_DEV      17
#define TRB_TYPE_NOOP_CMD       23
//vendor-specific command TRBs
#define TRB_NEC_GET_FW          49
//event TRBs
#define TRB_TYPE_XFER_EVT       32
#define TRB_TYPE_CMD_COMPLETE   33
#define TRB_TYPE_PORT_CHANGE    34
#define TRB_TYPE_BW_REQUEST     35
#define TRB_TYPE_HC_EVT         37

//TRB control field bits
#define TRB_C                   (1 << 0)    //cycle bit
#define TRB_TC                  (1 << 1)    //toggle cycle (link TRB)
#define TRB_ENT                 (1 << 1)    //evaluate next TRB
#define TRB_ISP                 (1 << 2)    //interrupt on short packet
#define TRB_NS                  (1 << 3)    //no snoop
#define TRB_CH                  (1 << 4)    //chain bit
#define TRB_IOC                 (1 << 5)    //interrupt on completion
#define TRB_IDT                 (1 << 6)    //immediate data (setup stage)
#define TRB_BSR                 (1 << 9)    //block set address request
#define TRB_DIR_IN              (1 << 16)   //direction IN (data/status stage)
#define TRB_TYPE_SHIFT          10
#define TRB_TYPE_MASK           (0x3F << TRB_TYPE_SHIFT)
#define TRB_SLOT_SHIFT          24          //slot ID field in command TRBs
#define TRB_EP_SHIFT            16          //endpoint index in command TRBs

//setup stage TRT (transfer type) field bits [17:16] in control dword
#define TRB_TRT_NO_DATA         (0 << 16)
#define TRB_TRT_OUT_DATA        (2 << 16)
#define TRB_TRT_IN_DATA         (3 << 16)

//TRB completion codes (bits [31:24] of status dword in event TRBs)
#define TRB_CC_INVALID          0
#define TRB_CC_SUCCESS          1
#define TRB_CC_DATA_BUF_ERR     2
#define TRB_CC_BABBLE           3
#define TRB_CC_USB_XACT_ERR     4
#define TRB_CC_TRB_ERR          5
#define TRB_CC_STALL            6
#define TRB_CC_SHORT_PKT        13
#define TRB_CC_RING_UNDERRUN    14
#define TRB_CC_RING_OVERRUN     15
#define TRB_CC_EVENT_RING_FULL  21
#define TRB_CC_EVENT_LOST       32

//endpoint context types (DW1 bits [5:3])
#define EP_TYPE_NOT_VALID       0
#define EP_TYPE_ISOCH_OUT       1
#define EP_TYPE_BULK_OUT        2
#define EP_TYPE_INTR_OUT        3
#define EP_TYPE_CTRL            4
#define EP_TYPE_ISOCH_IN        5
#define EP_TYPE_BULK_IN         6
#define EP_TYPE_INTR_IN         7

//device context index helpers
//EP0 (control bidir) = DCI 1
//EPn OUT = DCI 2n,  EPn IN = DCI 2n+1  (n = endpoint number 1-15)
#define EP_ADDR_TO_DCI(addr) \
    (((addr) & 0x0F) == 0 ? 1 : (((addr) & 0x0F) * 2 + (((addr) & 0x80) ? 1 : 0)))
#define DCI_EP0                 1
#define DCI_MAX                 31

//MSI-X
#define PCI_CAP_MSIX            0x11
#define XHCI_MSI_VECTOR         0x49    //interrupt vector for xHCI event ring (avoid NVMe 0x40-0x48)

//ring sizing
#define XHCI_CMD_RING_SIZE      32      //command ring TRBs (last = link)
#define XHCI_EVT_RING_SIZE      1024    //event ring TRBs (no link TRB; multi-page segment)
#define XHCI_TR_RING_SIZE       32      //transfer ring TRBs per endpoint (last = link)

//max devices / endpoints supported by this driver
#define XHCI_MAX_SLOTS          32
#define XHCI_MAX_EP             4       //EP0 + up to 3 additional endpoints per device

//max HID events to defer per interrupt (mouse + keyboard = 2 devices at most)
#define HID_PENDING_MAX         8

//controller quirks
#define XHCI_QUIRK_PORT_POLLING_RECOVER   (1U << 0)
#define XHCI_QUIRK_PORT_POLLING_WARM_RESET (1U << 1)
#define XHCI_QUIRK_FORCE_BIOS_HANDOFF     (1U << 2)
#define XHCI_QUIRK_NEC_HOST               (1U << 3)
#define XHCI_QUIRK_LINK_TRB_CHAIN         (1U << 4)
#define XHCI_QUIRK_RENESAS_FW_LOAD        (1U << 5)
#define XHCI_QUIRK_PANTHERPOINT           (1U << 6)
#define XHCI_QUIRK_INTEL_HOST             (1U << 7)

//deferred HID report - filled inside evt_lock, processed outside it
typedef struct {
    uint8       proto;          //HID_PROTO_KEYBOARD or _MOUSE
    uint8       slot;           //slot ID (to re-arm the interrupt TRB)
    uint16      len;            //actual report length
    uint8       data[64];       //copy of the report (boot protocol max is 8, generous)
} hid_pending_t;

//data structures

//a single 16-byte TRB
typedef struct __attribute__((packed)) {
    uint64 param;       //data pointer or command-specific
    uint32 status;      //transfer length / completion info
    uint32 control;     //cycle bit, type, flags, IDs
} xhci_trb_t;

//event ring segment table entry (16 bytes, 64-byte aligned)
typedef struct __attribute__((packed)) {
    uint64 base;        //physical base of the ring segment
    uint16 size;        //number of TRBs in segment
    uint8  pad[6];
} xhci_erst_entry_t;

//MSI-X table entry (16 bytes)
typedef struct __attribute__((packed)) {
    uint32 msg_addr_lo;
    uint32 msg_addr_hi;
    uint32 msg_data;
    uint32 vector_ctrl;  //bit 0 = masked
} xhci_msix_entry_t;

//single-segment TRB ring (used for command ring and per-endpoint transfer rings)
typedef struct {
    xhci_trb_t *trbs;      //virtual address of ring buffer
    uintptr     phys;       //physical base of ring buffer
    uint32      enq;        //enqueue index (producer writes here)
    uint32      deq;        //dequeue index (consumer / event ring only)
    uint32      size;       //total TRBs including Link TRB
    uint32      pages;      //allocated backing pages
    bool        chain_links; //set CH on link TRBs for older controllers
    uint8       pcs;        //producer cycle state bit
} xhci_ring_t;

//per-device/slot state
typedef struct {
    bool        in_use;
    uint8       slot_id;        //xHCI slot ID (1-based)
    uint8       port;           //0-based port index
    uint8       speed;          //USB_SPEED_* from PORTSC
    uint16      mps0;           //EP0 max packet size

    //output device context (written by HC)
    void       *out_ctx_phys;   //physical
    void       *out_ctx;        //virtual

    //transfer rings indexed by DCI-1 (so index 0 = DCI 1 = EP0)
    xhci_ring_t ep_ring[XHCI_MAX_EP];
    bool        ep_ring_ok[XHCI_MAX_EP];

    //synchronisation for synchronous control transfers
    volatile int ctrl_done;     //set to 1 by event handler when complete
    int          ctrl_cc;       //completion code
    uint32       ctrl_residual; //residual bytes from event TRB status

    //HID device info (filled during enumeration)
    bool        is_hid;
    uint8       hid_proto;      //USB_HID_PROTO_KEYBOARD or _MOUSE
    uint8       hid_ep_dci;     //DCI of the HID interrupt IN endpoint
    uint16      hid_ep_mps;     //max packet size of that endpoint
    uint8       hid_interval;   //bInterval from endpoint descriptor
    uint8       hid_iface;      //interface number
    bool        hid_intr_queued; //true while one interrupt-IN TRB is armed
    bool        hid_recovering; //true while endpoint reset/requeue is in progress

    //DMA buffer for interrupt transfer data
    void       *hid_buf_phys;
    void       *hid_buf;

    bool        disable_in_progress;
} xhci_device_t;

//per-controller state
typedef struct {
    pci_device_t  *pci;

    //virtual register bases (all MMIO, mapped NOCACHE)
    void          *cap_base;    //capability registers (= BAR0 virtual)
    void          *op_base;     //operational registers (= cap_base + CAPLENGTH)
    void          *rt_base;     //runtime registers
    uint32        *db_base;     //doorbell array

    //parsed capabilities
    uint16         hci_ver;     //xHCI version (e.g. 0x0096, 0x0100)
    uint8          ctx_size;    //bytes per context entry: 32 or 64
    uint8          max_ports;   //from HCSPARAMS1
    uint8          max_slots;   //from HCSPARAMS1 (capped to XHCI_MAX_SLOTS)

    //device context base address array
    uint64        *dcbaa;       //virtual
    uintptr        dcbaa_phys;  //physical

    //command ring
    xhci_ring_t    cmd_ring;
    spinlock_irq_t cmd_lock;    //serialises command submission

    //command completion (updated by event handler, polled or slept on)
    volatile int   cmd_done;
    int            cmd_cc;      //completion code of last command
    uint32         cmd_slot;    //slot ID returned by Enable Slot
    uintptr        last_cmd_phys; //physical addr of last submitted command TRB

    //event ring
    xhci_trb_t        *evt_ring;    //virtual
    uintptr            evt_ring_phys;
    xhci_erst_entry_t *erst;         //virtual: event ring segment table
    uintptr            erst_phys;
    uint32             evt_deq;      //consumer index into evt_ring
    uint8              evt_ccs;      //consumer cycle state (starts at 1)
    spinlock_irq_t     evt_lock;     //serialises event ring drain/ack across CPUs
    bool               hid_recovery_needed; //set when HC reports lost/full event ring

    //deferred HID reports (filled under evt_lock, processed after release)
    hid_pending_t      hid_pending[HID_PENDING_MAX];
    uint32             hid_pending_count;
    bool               disconnect_ports[256]; //pending port-disconnect cleanup
    uint32             quirks;

    //MSI-X
    uint16             msix_cap;     //byte offset of MSI-X cap in PCI config
    xhci_msix_entry_t *msix_table;   //virtual: MSI-X table

    //device slots (1-based; slot 0 unused)
    xhci_device_t devices[XHCI_MAX_SLOTS + 1];
} xhci_ctrl_t;

static inline bool xhci_has_quirk(const xhci_ctrl_t *c, uint32 quirk) {
    return c && (c->quirks & quirk);
}

//public interface
void xhci_init(void);
void xhci_irq(void);

#endif
