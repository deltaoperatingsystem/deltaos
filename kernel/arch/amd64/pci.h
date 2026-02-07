#ifndef ARCH_AMD64_PCI_H
#define ARCH_AMD64_PCI_H

#include <arch/amd64/types.h>

//MI PCI interface
uint32 arch_pci_read(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size);
void arch_pci_write(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size, uint32 value);

#endif
