#include <types.h>
#include <io/port.h>

#define PIC1            0x20
#define PIC2            0xA0
#define PIC1_CMD        PIC1
#define PIC1_DATA       (PIC1 + 1)
#define PIC2_CMD        PIC2
#define PIC2_DATA       (PIC2 + 1)

#define PIC_EOI         0x20

void pic_send_eoi(uint8 irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

#define ICW1_ICW4	    0x01		// Indicates that ICW4 will be present 
#define ICW1_SINGLE 	0x02		// Single (cascade) mode 
#define ICW1_INTERVAL4	0x04		// Call address interval 4 (8) 
#define ICW1_LEVEL	    0x08		// Level triggered (edge) mode 
#define ICW1_INIT	    0x10		// Initialization - required! 

#define ICW4_8086	    0x01		// 8086/88 (MCS-80/85) mode 
#define ICW4_AUTO	    0x02		// Auto (normal) EOI 
#define ICW4_BUF_SLAVE	0x08		// Buffered mode/slave 
#define ICW4_BUF_MASTER	0x0C		// Buffered mode/master 
#define ICW4_SFNM	    0x10		// Special fully nested (not) 

#define CASCADE_IRQ 2

// use this if/when switching to apic
void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_remap(uint8 vector1, uint8 vector2) {
	outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  // starts the initialization sequence (in cascade mode)
	outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);

	outb(PIC1_DATA, vector1);                 // ICW2: Master PIC vector offset
	outb(PIC2_DATA, vector2);                 // ICW2: Slave PIC vector offset
	outb(PIC1_DATA, 1 << CASCADE_IRQ);        // ICW3: tell Master PIC that there is a slave PIC at IRQ2
	outb(PIC2_DATA, 2);                       // ICW3: tell Slave PIC its cascade identity (0000 0010)
	
	outb(PIC1_DATA, ICW4_8086);               // ICW4: have the PICs use 8086 mode (and not 8080 mode)
	outb(PIC2_DATA, ICW4_8086);

	// Disable it for now
    pic_disable();
}

void pic_set_mask(uint8 irqline) {
    uint16 port;

    if (irqline < 8) port = PIC1_DATA;
    else {
        port = PIC2_DATA;
        irqline -= 8;
    }
    uint8 value = inb(port) | (1 << irqline);
    outb(port, value);
}

void pic_clear_mask(uint8 irqline) {
    uint16 port;

    if (irqline < 8) port = PIC1_DATA;
    else {
        port = PIC2_DATA;
        irqline -= 8;
    }
    uint8 value = inb(port) & ~(1 << irqline);
    outb(port, value);
}