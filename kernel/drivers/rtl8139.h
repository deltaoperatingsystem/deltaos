#ifndef DRIVERS_RTL8139_H
#define DRIVERS_RTL8139_H

#include <arch/types.h>

//RTL8139 PCI IDs
#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

//RTL8139 register offsets (I/O port base)
#define RTL_IDR0       0x00   //MAC address bytes 0-3
#define RTL_IDR4       0x04   //MAC address bytes 4-5
#define RTL_MAR0       0x08   //multicast address register 0-3
#define RTL_MAR4       0x0C   //multicast address register 4-7
#define RTL_TSD0       0x10   //transmit status descriptor 0 (+ 4*n for 1-3)
#define RTL_TSAD0      0x20   //transmit start address descriptor 0 (+ 4*n for 1-3)
#define RTL_RBSTART    0x30   //receive buffer start address
#define RTL_CMD        0x37   //command register
#define RTL_CAPR       0x38   //current address of packet read (read pointer)
#define RTL_CBR        0x3A   //current buffer address (write pointer)
#define RTL_IMR        0x3C   //interrupt mask register
#define RTL_ISR        0x3E   //interrupt status register
#define RTL_TCR        0x40   //transmit configuration register
#define RTL_RCR        0x44   //receive configuration register
#define RTL_CONFIG1    0x52   //configuration register 1

//command register bits
#define RTL_CMD_RESET  (1 << 4)
#define RTL_CMD_RE     (1 << 3)  //receiver enable
#define RTL_CMD_TE     (1 << 2)  //transmitter enable

//interrupt bits (for IMR/ISR)
#define RTL_INT_ROK    (1 << 0)  //receive OK
#define RTL_INT_RER    (1 << 1)  //receive error
#define RTL_INT_TOK    (1 << 2)  //transmit OK
#define RTL_INT_TER    (1 << 3)  //transmit error
#define RTL_INT_RXOVW  (1 << 4)  //RX buffer overflow

//receive configuration register bits
#define RTL_RCR_AAP    (1 << 0)  //accept all packets
#define RTL_RCR_APM    (1 << 1)  //accept physical match
#define RTL_RCR_AM     (1 << 2)  //accept multicast
#define RTL_RCR_AB     (1 << 3)  //accept broadcast
#define RTL_RCR_WRAP   (1 << 7)  //wrap around

//receive buffer sizes
#define RTL_RXBUF_8K   0         //8K + 16
#define RTL_RXBUF_16K  (1 << 11) //16K + 16
#define RTL_RXBUF_32K  (2 << 11) //32K + 16
#define RTL_RXBUF_64K  (3 << 11) //64K + 16

//transmit status bits
#define RTL_TSD_OWN    (1 << 13) //DMA operation complete
#define RTL_TSD_TOK    (1 << 15) //transmit OK

//buffer sizes
#define RTL_RX_BUF_SIZE  (8192 + 16 + 1500)  //8K + header + max packet
#define RTL_TX_BUF_SIZE  2048

//number of TX descriptors
#define RTL_NUM_TX_DESC  4

//driver interface
void rtl8139_init(void);
void rtl8139_irq(void);

#endif
