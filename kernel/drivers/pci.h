#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <arch/types.h>

//PCI device info structure
typedef struct pci_device {
    uint16 vendor_id;
    uint16 device_id;
    uint16 command;
    uint16 status;
    uint8  class_code;
    uint8  subclass;
    uint8  prog_if;
    uint8  header_type;
    uint8  int_line;
    uint8  int_pin;
    
    uint8  bus;
    uint8  dev;
    uint8  func;
    
    //BARs (base address registers)
    struct {
        uint64 addr;
        uint64 size;
        bool   is_io;
        bool   is_64bit;
    } bar[6];
    
    struct pci_device *next;  //linked list
} pci_device_t;

//initialize PCI subsystem so enumerate and register devices
void pci_init(void);

//find device by vendor/device ID (returns first match and NULL if not found)
pci_device_t *pci_find_device(uint16 vendor_id, uint16 device_id);

//find device by class/subclass (returns first match and NULL if not found)
pci_device_t *pci_find_class(uint8 class_code, uint8 subclass);

//get device by BDF address
pci_device_t *pci_get_device(uint8 bus, uint8 dev, uint8 func);

//get list of all devices (head of linked list)
pci_device_t *pci_get_devices(void);

//count of discovered devices
uint32 pci_device_count(void);

//raw config space access (for drivers)
uint32 pci_config_read(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size);
void   pci_config_write(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size, uint32 value);

//enable bus mastering for a device
void pci_enable_bus_master(pci_device_t *dev);

//enable memory space access for a device
void pci_enable_mmio(pci_device_t *dev);

//enable I/O space access for a device
void pci_enable_io(pci_device_t *dev);

#endif