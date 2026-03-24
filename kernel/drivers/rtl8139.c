#include <drivers/rtl8139.h>
#include <drivers/pci.h>
#include <drivers/init.h>
#include <arch/io.h>
#include <arch/interrupts.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <net/net.h>
#include <net/ethernet.h>

typedef struct rtl8139_dev rtl8139_dev_t;

struct rtl8139_dev {
    rtl8139_dev_t *next;
    pci_device_t *pci;
    uint16 io_base;

    //MAC address
    uint8 mac[6];

    //receive buffer (physically contiguous)
    uint8 *rx_buf;         //virtual address
    uintptr rx_buf_phys;   //physical address
    uint16 rx_cur;         //current offset in RX buffer
    uint32 rx_config;      //cached RCR value
    spinlock_irq_t rx_lock;

    //transmit buffers
    uint8 *tx_buf[RTL_NUM_TX_DESC];       //virtual
    uintptr tx_buf_phys[RTL_NUM_TX_DESC]; //physical
    uint8 tx_cur;                          //current TX descriptor index
    bool  tx_used[RTL_NUM_TX_DESC];        //track if descriptor has been used before
    spinlock_irq_t tx_lock;
    bool started;

    //netif for the stack
    netif_t netif;
};

#define RTL_RX_RING_SIZE 32768

static rtl8139_dev_t *dev_list = NULL;
static uint32 dev_count = 0;

static int rtl8139_netif_send(netif_t *nif, const void *data, size len);
static void rtl8139_handle_rx(rtl8139_dev_t *d);
static void rtl8139_service_device(rtl8139_dev_t *d);

static void rtl8139_poll_hook(netif_t *nif) {
    (void)nif;
    rtl8139_poll();
}

static void *rtl8139_alloc_dma_pages(size pages, uintptr *phys_out) {
    void *phys = pmm_alloc_zone(pages, 0xFFFFFFFF);
    if (!phys) return NULL;

    if (phys_out) *phys_out = (uintptr)phys;
    return P2V(phys);
}

static void rtl8139_reset(rtl8139_dev_t *d) {
    //power on
    outb(d->io_base + RTL_CONFIG1, 0x00);

    //software reset
    outb(d->io_base + RTL_CMD, RTL_CMD_RESET);

    //wait for reset to complete (RST bit clears)
    int timeout = 100000;
    while ((inb(d->io_base + RTL_CMD) & RTL_CMD_RESET) && --timeout) {
        //spin
    }

    if (timeout <= 0) {
        printf("[rtl8139] WARNING: Reset timed out\n");
    }
}

static void rtl8139_read_mac(rtl8139_dev_t *d) {
    uint32 mac_lo = inl(d->io_base + RTL_IDR0);
    uint16 mac_hi = inw(d->io_base + RTL_IDR4);

    d->mac[0] = mac_lo & 0xFF;
    d->mac[1] = (mac_lo >> 8) & 0xFF;
    d->mac[2] = (mac_lo >> 16) & 0xFF;
    d->mac[3] = (mac_lo >> 24) & 0xFF;
    d->mac[4] = mac_hi & 0xFF;
    d->mac[5] = (mac_hi >> 8) & 0xFF;
}

static int rtl8139_setup_rx(rtl8139_dev_t *d) {
    //allocate RX buffer (needs to be physically contiguous)
    //RTL_RX_BUF_SIZE = 8192 + 16 + 1500 = 9708 bytes -> 3 pages
    size rx_pages = (RTL_RX_BUF_SIZE + 4095) / 4096;
    d->rx_buf = rtl8139_alloc_dma_pages(rx_pages, &d->rx_buf_phys);
    if (!d->rx_buf) {
        printf("[rtl8139] ERROR: Failed to allocate RX buffer\n");
        return -1;
    }

    d->rx_cur = 0;
    spinlock_irq_init(&d->rx_lock);

    memset(d->rx_buf, 0, RTL_RX_BUF_SIZE);
    printf("[rtl8139] RX buffer phys=0x%lx pages=%u\n", (unsigned long)d->rx_buf_phys, (uint32)rx_pages);

    return 0;
}

static int rtl8139_setup_tx(rtl8139_dev_t *d) {
    //allocate TX buffers (one page each, physically contiguous)
    for (int i = 0; i < RTL_NUM_TX_DESC; i++) {
        uintptr tx_phys = 0;
        d->tx_buf[i] = rtl8139_alloc_dma_pages(1, &tx_phys);
        if (!d->tx_buf[i]) {
            printf("[rtl8139] ERROR: Failed to allocate TX buffer %d\n", i);
            //cleanup previously allocated buffers
            for (int j = 0; j < i; j++) {
                pmm_free((void*)d->tx_buf_phys[j], 1);
            }
            return -1;
        }

        d->tx_buf_phys[i] = tx_phys;
        memset(d->tx_buf[i], 0, RTL_TX_BUF_SIZE);
        printf("[rtl8139] TX buffer %d phys=0x%lx\n", i, (unsigned long)d->tx_buf_phys[i]);

        //set TX start address for this descriptor
        outl(d->io_base + RTL_TSAD0 + (i * 4), (uint32)d->tx_buf_phys[i]);
    }
    d->tx_cur = 0;
    spinlock_irq_init(&d->tx_lock);
    return 0;
}

static void rtl8139_enable(rtl8139_dev_t *d) {
    //program the receive ring before enabling the datapath
    outl(d->io_base + RTL_RBSTART, (uint32)d->rx_buf_phys);

    //use a generous FIFO/DMA burst
    d->rx_config = RTL_RCR_AAP | RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AM |
                   RTL_RCR_WRAP | RTL_RCR_FIFO_EOP | RTL_RCR_DMA_EOP |
                   RTL_RXBUF_32K;
    outl(d->io_base + RTL_RCR, d->rx_config);
    //initialize CAPR
    //the hardware expects this to start at 0xFFF0
    outw(d->io_base + RTL_CAPR, 0xFFF0);

    //set interrupt mask - we want ROK, TOK, RX errors, RX overflow
    outw(d->io_base + RTL_IMR,
         RTL_INT_ROK | RTL_INT_TOK | RTL_INT_RER | RTL_INT_TER | RTL_INT_RXOVW);

    //enable transmitter and receiver after the ring/config are live
    outb(d->io_base + RTL_CMD, RTL_CMD_TE | RTL_CMD_RE);
}

static int rtl8139_transmit(rtl8139_dev_t *d, const void *data, size len) {
    if (len > RTL_TX_BUF_SIZE) return -1;

    irq_state_t flags = spinlock_irq_acquire(&d->tx_lock);

    uint8 desc = d->tx_cur;

    //wait for this descriptor to be free (OWN bit set = DMA done)
    int timeout = 100000;
    while (!(inl(d->io_base + RTL_TSD0 + (desc * 4)) & RTL_TSD_OWN) && --timeout) {
        //descriptors start unowned, so skip check on the very first use
        if (!d->tx_used[desc]) break;
    }

    //copy data to TX buffer
    memcpy(d->tx_buf[desc], data, len);

    //mark as used
    d->tx_used[desc] = true;

    //write the transmit status: clear OWN bit and set size
    //size goes in bits 0-12, we clear bit 13 (OWN) to start DMA
    outl(d->io_base + RTL_TSD0 + (desc * 4), (uint32)len);

    //advance to next descriptor
    d->tx_cur = (desc + 1) % RTL_NUM_TX_DESC;

    spinlock_irq_release(&d->tx_lock, flags);
    return 0;
}

static void rtl8139_service_device(rtl8139_dev_t *d) {
    if (!d || !d->started) return;

    uint16 status = inw(d->io_base + RTL_ISR);
    if (status == 0) return;

    //acknowledge all interrupts
    outw(d->io_base + RTL_ISR, status);

    if (status & RTL_INT_ROK) {
        rtl8139_handle_rx(d);
    }

    if (status & RTL_INT_TOK) {
        //TX complete - nothing to do for now
    }

    if (status & RTL_INT_RXOVW) {
        printf("[rtl8139] RX buffer overflow!\n");
        //reset the receiver, toggle RxEnb and reprogram the receive config
        uint8 cmd = inb(d->io_base + RTL_CMD);
        outb(d->io_base + RTL_CMD, cmd & ~RTL_CMD_RE);
        outb(d->io_base + RTL_CMD, cmd);
        outl(d->io_base + RTL_RCR, d->rx_config);
        d->rx_cur = 0;
        outw(d->io_base + RTL_CAPR, 0xFFF0);
    }

    if (status & (RTL_INT_RER | RTL_INT_TER)) {
        printf("[rtl8139] Error (ISR=0x%04x)\n", status);
    }
}

static void rtl8139_handle_rx(rtl8139_dev_t *d) {
    irq_state_t flags = spinlock_irq_acquire(&d->rx_lock);
    while (!(inb(d->io_base + RTL_CMD) & 0x01)) { //buffer not empty
        //RTL8139 RX packet format:
        //[status:16][length:16][packet data...][padding to dword]
        uint8 *buf = d->rx_buf + d->rx_cur;
        uint32 hdr = (uint32)buf[0] |
                     ((uint32)buf[1] << 8) |
                     ((uint32)buf[2] << 16) |
                     ((uint32)buf[3] << 24);
        uint16 status = hdr & 0xFFFF;
        uint16 length = hdr >> 16;

        if (!(status & 0x0001)) {
            //bad packet, skip
            printf("[rtl8139] RX error (status=0x%04x)\n", status);
            break;
        }

        if (length < 4 || length > ETH_MTU + 18) {
            //invalid length
            printf("[rtl8139] RX invalid length=%u\n", (uint32)length);
            break;
        }

        //pass packet up to network stack (skip the 4-byte RTL header)
        net_rx(&d->netif, buf + 4, length - 4);  //-4 for CRC

        //advance read pointer (aligned to dword)
        d->rx_cur = (d->rx_cur + length + 4 + 3) & ~3;
        d->rx_cur &= (RTL_RX_RING_SIZE - 1);

        //update CAPR (read pointer) - RTL wants CAPR = cur - 16
        outw(d->io_base + RTL_CAPR, d->rx_cur - 16);
    }
    spinlock_irq_release(&d->rx_lock, flags);
}

void rtl8139_poll(void) {
    for (rtl8139_dev_t *d = dev_list; d; d = d->next) {
        rtl8139_service_device(d);
    }
}

//ISR called from interrupt dispatcher
void rtl8139_irq(void) {
    for (rtl8139_dev_t *d = dev_list; d; d = d->next) {
        rtl8139_service_device(d);
    }
}

//netif send callback
static int rtl8139_netif_send(netif_t *nif, const void *data, size len) {
    rtl8139_dev_t *d = (rtl8139_dev_t *)nif->driver_data;
    return rtl8139_transmit(d, data, len);
}

static void rtl8139_init_device(pci_device_t *pci) {
    rtl8139_dev_t *dev = kzalloc(sizeof(rtl8139_dev_t));
    if (!dev) return;

    dev->pci = pci;

    //enable PCI bus master and I/O space
    pci_enable_bus_master(pci);
    pci_enable_io(pci);

    //get I/O base from BAR0
    dev->io_base = (uint16)(pci->bar[0].addr & 0xFFFF);

    printf("[rtl8139] Found at PCI %u:%u.%u, I/O base: 0x%04x\n",
           pci->bus, pci->dev, pci->func, dev->io_base);
    printf("[rtl8139] PCI INTx line %u pin %u\n", pci->int_line, pci->int_pin);

    //reset the chip
    rtl8139_reset(dev);

    //read MAC address
    rtl8139_read_mac(dev);
    printf("[rtl8139] MAC: ");
    net_print_mac(dev->mac);
    printf("\n");

    //setup buffers
    if (rtl8139_setup_rx(dev) != 0 || rtl8139_setup_tx(dev) != 0) {
        printf("[rtl8139] ERROR: Buffer setup failed, disabling device\n");
        //cleanup is handled inside setup_tx for TX, and we could add it for RX
        //but for now just bail to avoid crashing
        return;
    }

    //enable TX/RX and interrupts
    rtl8139_enable(dev);

    if (pci->int_line != 0xFF) {
        interrupt_register(pci->int_line, rtl8139_irq);
        interrupt_unmask(pci->int_line);
    }

    //register with the network stack
    netif_t *nif = &dev->netif;
    snprintf(nif->name, sizeof(nif->name), "eth%u", dev_count);
    memcpy(nif->mac, dev->mac, 6);
    nif->ip_addr = 0; //unconfigured but DHCP will set this
    nif->subnet_mask = 0;
    nif->gateway = 0;
    nif->up = true;
    nif->send = rtl8139_netif_send;
    nif->poll = rtl8139_poll_hook;
    nif->driver_data = dev;

    dev->next = dev_list;
    dev_list = dev;
    dev_count++;

    net_register_netif(nif);
    dev->started = true;

    printf("[rtl8139] Driver initialized (%u total)\n", dev_count);
}

void rtl8139_init(void) {
    //search for RTL8139 on the PCI bus
    uint32 found = 0;
    for (pci_device_t *pci = pci_get_devices(); pci; pci = pci->next) {
        if (pci->vendor_id == RTL8139_VENDOR_ID && pci->device_id == RTL8139_DEVICE_ID) {
            rtl8139_init_device(pci);
            found++;
        }
    }
    if (found == 0) {
        printf("[rtl8139] No RTL8139 NIC found\n");
    }
}

DECLARE_DRIVER(rtl8139_init, INIT_LEVEL_DEVICE);
