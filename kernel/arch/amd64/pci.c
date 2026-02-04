#include <arch/pci.h>
#include <arch/amd64/acpi/acpi.h>
#include <arch/amd64/io.h>
#include <mm/mm.h>
#include <lib/io.h>

//PCI config space ports
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

//port I/O helpers
static uint32 pci_legacy_make_addr(uint8 bus, uint8 dev, uint8 func, uint8 offset) {
    return (1u << 31) //enable bit
         | ((uint32)bus << 16)
         | ((uint32)dev << 11)
         | ((uint32)func << 8)
         | (offset & 0xFC);
}

static uint32 pci_legacy_read(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size) {
    outl(PCI_CONFIG_ADDR, pci_legacy_make_addr(bus, dev, func, offset));
    uint32 dword = inl(PCI_CONFIG_DATA);
    uint32 shift = (offset & 3) * 8;
    
    switch (size) {
        case 1: return (dword >> shift) & 0xFF;
        case 2: return (dword >> shift) & 0xFFFF;
        case 4: return dword;
        default: return 0;
    }
}

static void pci_legacy_write(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size, uint32 value) {
    uint32 addr = pci_legacy_make_addr(bus, dev, func, offset);
    outl(PCI_CONFIG_ADDR, addr);
    
    if (size == 4) {
        outl(PCI_CONFIG_DATA, value);
    } else {
        uint32 dword = inl(PCI_CONFIG_DATA);
        uint32 shift = (offset & 3) * 8;
        uint32 mask;
        
        switch (size) {
            case 1: mask = 0xFF << shift; break;
            case 2: mask = 0xFFFF << shift; break;
            default: return;
        }
        
        dword = (dword & ~mask) | ((value << shift) & mask);
        outl(PCI_CONFIG_ADDR, addr); //re-write addr just in case
        outl(PCI_CONFIG_DATA, dword);
    }
}

//ECAM (MMIO) helpers
static void *pci_ecam_get_addr(uint8 bus, uint8 dev, uint8 func, uint8 offset) {
    if (acpi_mcfg_addr == 0) return NULL;
    if (bus < acpi_mcfg_start_bus || bus > acpi_mcfg_end_bus) return NULL;
    
    uintptr phys = acpi_mcfg_addr 
                 + ((((uintptr)bus - acpi_mcfg_start_bus) << 20)
                 | ((uintptr)dev << 15)
                 | ((uintptr)func << 12)
                 | (uintptr)offset);
                 
    return P2V(phys);
}

uint32 arch_pci_read(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size) {
    void *mmio_addr = pci_ecam_get_addr(bus, dev, func, offset);
    if (mmio_addr) {
        switch (size) {
            case 1: return *(volatile uint8 *)mmio_addr;
            case 2: return *(volatile uint16 *)mmio_addr;
            case 4: return *(volatile uint32 *)mmio_addr;
            default: return 0;
        }
    }
    
    return pci_legacy_read(bus, dev, func, offset, size);
}

void arch_pci_write(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size, uint32 value) {
    void *mmio_addr = pci_ecam_get_addr(bus, dev, func, offset);
    if (mmio_addr) {
        switch (size) {
            case 1: *(volatile uint8 *)mmio_addr = (uint8)value; break;
            case 2: *(volatile uint16 *)mmio_addr = (uint16)value; break;
            case 4: *(volatile uint32 *)mmio_addr = (uint32)value; break;
        }
        return;
    }
    
    pci_legacy_write(bus, dev, func, offset, size, value);
}
