#ifndef ARCH_AMD64_APIC_H
#define ARCH_AMD64_APIC_H

#include <arch/amd64/types.h>

//local APIC register offsets
#define APIC_ID                 0x0020  //local APIC ID
#define APIC_VERSION            0x0030  //local APIC version
#define APIC_TPR                0x0080  //task priority register
#define APIC_EOI                0x00B0  //end of interrupt
#define APIC_LDR                0x00D0  //logical destination register
#define APIC_DFR                0x00E0  //destination format register
#define APIC_SPURIOUS           0x00F0  //spurious interrupt vector register
#define APIC_ESR                0x0280  //error status register
#define APIC_ICR_LOW            0x0300  //interrupt command register (low)
#define APIC_ICR_HIGH           0x0310  //interrupt command register (high)
#define APIC_LVT_TIMER          0x0320  //LVT timer register
#define APIC_LVT_LINT0          0x0350  //LVT LINT0 register
#define APIC_LVT_LINT1          0x0360  //LVT LINT1 register
#define APIC_LVT_ERROR          0x0370  //LVT error register
#define APIC_TIMER_ICR          0x0380  //timer initial count register
#define APIC_TIMER_CCR          0x0390  //timer current count register
#define APIC_TIMER_DCR          0x03E0  //timer divide configuration register

//spurious interrupt vector register bits
#define APIC_SPURIOUS_ENABLE    (1 << 8)

//LVT timer bits
#define APIC_TIMER_PERIODIC     (1 << 17)
#define APIC_TIMER_MASKED       (1 << 16)

//timer divide values
#define APIC_TIMER_DIV_1        0xB
#define APIC_TIMER_DIV_2        0x0
#define APIC_TIMER_DIV_4        0x1
#define APIC_TIMER_DIV_8        0x2
#define APIC_TIMER_DIV_16       0x3
#define APIC_TIMER_DIV_32       0x8
#define APIC_TIMER_DIV_64       0x9
#define APIC_TIMER_DIV_128      0xA

//MSR addresses
#define MSR_APIC_BASE           0x1B
#define APIC_BASE_ENABLE        (1 << 11)

//default vectors
#define APIC_SPURIOUS_VECTOR    0xFF
#define APIC_TIMER_VECTOR       0x20

//interface
bool apic_is_supported(void);
bool apic_init(void);
void apic_set_force_pic(bool force);
void apic_send_eoi(void);
bool apic_is_enabled(void);
uint32 apic_get_id(void);
uint32 apic_read(uint32 reg);
void apic_write(uint32 reg, uint32 val);
void apic_timer_init(uint32 hz);

#endif
