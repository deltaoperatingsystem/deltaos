#include <drivers/rtl8139.h>
#include <drivers/pci.h>
#include <drivers/init.h>
#include <arch/io.h>
#include <mm/pmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <lib/io.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <net/net.h>
#include <net/ethernet.h>

typedef struct {
    pci_device_t *pci;
    uint16 io_base;
    
    //MAC address
    uint8 mac[6];
    
    //receive buffer (physically contiguous)
    uint8 *rx_buf;         //virtual address
    uintptr rx_buf_phys;   //physical address
    uint16 rx_cur;         //current offset in RX buffer
    
    //transmit buffers
    uint8 *tx_buf[RTL_NUM_TX_DESC];       //virtual
    uintptr tx_buf_phys[RTL_NUM_TX_DESC]; //physical
    uint8 tx_cur;                          //current TX descriptor index
    spinlock_irq_t tx_lock;
    
    //netif for the stack
    netif_t netif;
} rtl8139_dev_t;

static rtl8139_dev_t *dev = NULL;

static int rtl8139_netif_send(netif_t *nif, const void *data, size len);

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
    void *rx_phys = pmm_alloc(rx_pages);
    if (!rx_phys) {
        printf("[rtl8139] ERROR: Failed to allocate RX buffer\n");
        return -1;
    }
    
    d->rx_buf = P2V(rx_phys);
    d->rx_buf_phys = (uintptr)rx_phys;
    d->rx_cur = 0;
    
    memset(d->rx_buf, 0, RTL_RX_BUF_SIZE);
    
    //tell the NIC where the RX buffer is
    outl(d->io_base + RTL_RBSTART, (uint32)d->rx_buf_phys);
    
    //configure RX: accept broadcast + physical match + multicast, wrap, 8K buffer
    outl(d->io_base + RTL_RCR,
         RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_WRAP | RTL_RXBUF_8K);
    
    return 0;
}

static int rtl8139_setup_tx(rtl8139_dev_t *d) {
    //allocate TX buffers (one page each, physically contiguous)
    for (int i = 0; i < RTL_NUM_TX_DESC; i++) {
        void *tx_phys = pmm_alloc(1);
        if (!tx_phys) {
            printf("[rtl8139] ERROR: Failed to allocate TX buffer %d\n", i);
            //cleanup previously allocated buffers
            for (int j = 0; j < i; j++) {
                pmm_free((void*)d->tx_buf_phys[j], 1);
            }
            return -1;
        }
        
        d->tx_buf[i] = P2V(tx_phys);
        d->tx_buf_phys[i] = (uintptr)tx_phys;
        memset(d->tx_buf[i], 0, RTL_TX_BUF_SIZE);
        
        //set TX start address for this descriptor
        outl(d->io_base + RTL_TSAD0 + (i * 4), (uint32)d->tx_buf_phys[i]);
    }
    d->tx_cur = 0;
    spinlock_irq_init(&d->tx_lock);
    return 0;
}

static void rtl8139_enable(rtl8139_dev_t *d) {
    //enable transmitter and receiver
    outb(d->io_base + RTL_CMD, RTL_CMD_TE | RTL_CMD_RE);
    
    //set interrupt mask — we want ROK, TOK, RX errors, RX overflow
    outw(d->io_base + RTL_IMR,
         RTL_INT_ROK | RTL_INT_TOK | RTL_INT_RER | RTL_INT_TER | RTL_INT_RXOVW);
}

static int rtl8139_transmit(rtl8139_dev_t *d, const void *data, size len) {
    if (len > RTL_TX_BUF_SIZE) return -1;
    
    spinlock_irq_acquire(&d->tx_lock);
    
    uint8 desc = d->tx_cur;
    
    //wait for this descriptor to be free (OWN bit set = DMA done)
    int timeout = 100000;
    while (!(inl(d->io_base + RTL_TSD0 + (desc * 4)) & RTL_TSD_OWN) && --timeout) {
        //first descriptor starts unowned, so skip check on the very first send
        if (timeout == 99999) break;
    }
    
    //copy data to TX buffer
    memcpy(d->tx_buf[desc], data, len);
    
    //write the transmit status: clear OWN bit and set size
    //size goes in bits 0-12, we clear bit 13 (OWN) to start DMA
    outl(d->io_base + RTL_TSD0 + (desc * 4), (uint32)len);
    
    //advance to next descriptor
    d->tx_cur = (desc + 1) % RTL_NUM_TX_DESC;
    
    spinlock_irq_release(&d->tx_lock);
    return 0;
}

static void rtl8139_handle_rx(rtl8139_dev_t *d) {
    while (!(inb(d->io_base + RTL_CMD) & 0x01)) { //buffer not empty
        //RTL8139 RX packet format:
        //[status:16][length:16][packet data...][padding to dword]
        uint8 *buf = d->rx_buf + d->rx_cur;
        uint16 status = *(uint16 *)(buf);
        uint16 length = *(uint16 *)(buf + 2);
        
        if (!(status & 0x01)) {
            //bad packet, skip
            printf("[rtl8139] RX error (status=0x%04x)\n", status);
            break;
        }
        
        if (length == 0 || length > ETH_MTU + 18) {
            //invalid length
            break;
        }
        
        //pass packet up to network stack (skip the 4-byte RTL header)
        net_rx(&d->netif, buf + 4, length - 4);  //-4 for CRC
        
        //advance read pointer (aligned to dword)
        d->rx_cur = (d->rx_cur + length + 4 + 3) & ~3;
        d->rx_cur %= RTL_RX_BUF_SIZE;
        
        //update CAPR (read pointer) - RTL wants CAPR = cur - 16
        outw(d->io_base + RTL_CAPR, d->rx_cur - 16);
    }
}

//ISR called from interrupt dispatcher
void rtl8139_irq(void) {
    if (!dev) return;
    
    uint16 status = inw(dev->io_base + RTL_ISR);
    if (status == 0) return;
    
    //acknowledge all interrupts
    outw(dev->io_base + RTL_ISR, status);
    
    if (status & RTL_INT_ROK) {
        rtl8139_handle_rx(dev);
    }
    
    if (status & RTL_INT_TOK) {
        //TX complete - nothing to do for now
    }
    
    if (status & RTL_INT_RXOVW) {
        printf("[rtl8139] RX buffer overflow!\n");
        //reset the RX by re-enabling 
        outb(dev->io_base + RTL_CMD, RTL_CMD_TE | RTL_CMD_RE);
    }
    
    if (status & (RTL_INT_RER | RTL_INT_TER)) {
        printf("[rtl8139] Error (ISR=0x%04x)\n", status);
    }
}

//netif send callback
static int rtl8139_netif_send(netif_t *nif, const void *data, size len) {
    rtl8139_dev_t *d = (rtl8139_dev_t *)nif->driver_data;
    return rtl8139_transmit(d, data, len);
}

static void rtl8139_init_device(pci_device_t *pci) {
    dev = kzalloc(sizeof(rtl8139_dev_t));
    if (!dev) return;
    
    dev->pci = pci;
    
    //enable PCI bus master and I/O space
    pci_enable_bus_master(pci);
    pci_enable_io(pci);
    
    //get I/O base from BAR0
    dev->io_base = (uint16)(pci->bar[0].addr & 0xFFFF);
    
    printf("[rtl8139] Found at PCI %u:%u.%u, I/O base: 0x%04x\n",
           pci->bus, pci->dev, pci->func, dev->io_base);
    
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
    
    //register with the network stack
    netif_t *nif = &dev->netif;
    strcpy(nif->name, "eth0");
    memcpy(nif->mac, dev->mac, 6);
    nif->ip_addr = 0; //unconfigured but DHCP will set this
    nif->subnet_mask = 0;
    nif->gateway = 0;
    nif->up = true;
    nif->send = rtl8139_netif_send;
    nif->driver_data = dev;
    
    net_register_netif(nif);
    
    printf("[rtl8139] Driver initialized\n");
}

void rtl8139_init(void) {
    //search for RTL8139 on the PCI bus
    pci_device_t *pci = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!pci) {
        printf("[rtl8139] No RTL8139 NIC found\n");
        return;
    }
    
    rtl8139_init_device(pci);
}

DECLARE_DRIVER(rtl8139_init, INIT_LEVEL_DEVICE);
