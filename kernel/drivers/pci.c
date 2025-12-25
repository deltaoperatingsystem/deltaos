//this is an inherently amd64/x86 driver because it relies on port I/O

#include <drivers/pci.h>
#include <arch/io.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <lib/io.h>
#include <lib/string.h>

//PCI config space ports
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

//PCI command register bits
#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_MEM_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER   (1 << 2)

//PCI header type bits
#define PCI_HEADER_TYPE_MASK     0x7F
#define PCI_HEADER_MULTIFUNCTION 0x80

//PCI class codes for bridges
#define PCI_CLASS_BRIDGE     0x06
#define PCI_SUBCLASS_PCI2PCI 0x04

//device list
static pci_device_t *device_list = NULL;
static uint32 device_count = 0;

//class code names for logging
static const char *pci_class_name(uint8 class_code) {
    switch (class_code) {
        case 0x00: return "Unclassified";
        case 0x01: return "Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Communication";
        case 0x08: return "System";
        case 0x09: return "Input";
        case 0x0A: return "Docking";
        case 0x0B: return "Processor";
        case 0x0C: return "Serial Bus";
        case 0x0D: return "Wireless";
        case 0x0E: return "Intelligent";
        case 0x0F: return "Satellite";
        case 0x10: return "Encryption";
        case 0x11: return "Signal Processing";
        default:   return "Unknown";
    }
}

static void pci_scan_bus(uint8 bus);


//build config address
static uint32 pci_make_addr(uint8 bus, uint8 dev, uint8 func, uint8 offset) {
    return (1u << 31)           //enable bit
         | ((uint32)bus << 16)
         | ((uint32)dev << 11)
         | ((uint32)func << 8)
         | (offset & 0xFC);
}

//raw config space read (dword aligned)
static uint32 pci_read_dword(uint8 bus, uint8 dev, uint8 func, uint8 offset) {
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

//raw config space write (dword aligned)
static void pci_write_dword(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint32 value) {
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

//public config read with size support
uint32 pci_config_read(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size) {
    uint32 dword = pci_read_dword(bus, dev, func, offset);
    uint32 shift = (offset & 3) * 8;
    
    switch (size) {
        case 1: return (dword >> shift) & 0xFF;
        case 2: return (dword >> shift) & 0xFFFF;
        case 4: return dword;
        default: return 0;
    }
}

//public config write with size support
void pci_config_write(uint8 bus, uint8 dev, uint8 func, uint8 offset, uint8 size, uint32 value) {
    uint32 dword = pci_read_dword(bus, dev, func, offset);
    uint32 shift = (offset & 3) * 8;
    uint32 mask;
    
    switch (size) {
        case 1: mask = 0xFF << shift; break;
        case 2: mask = 0xFFFF << shift; break;
        case 4: mask = 0xFFFFFFFF; break;
        default: return;
    }
    
    dword = (dword & ~mask) | ((value << shift) & mask);
    pci_write_dword(bus, dev, func, offset, dword);
}

//probe BARs for a device
static void pci_probe_bars(pci_device_t *pdev) {
    for (int i = 0; i < 6; i++) {
        uint8 offset = 0x10 + (i * 4);
        uint32 orig = pci_read_dword(pdev->bus, pdev->dev, pdev->func, offset);
        
        if (orig == 0) {
            pdev->bar[i].addr = 0;
            pdev->bar[i].size = 0;
            pdev->bar[i].is_io = false;
            pdev->bar[i].is_64bit = false;
            continue;
        }
        
        //write all 1s to determine size
        pci_write_dword(pdev->bus, pdev->dev, pdev->func, offset, 0xFFFFFFFF);
        uint32 sized = pci_read_dword(pdev->bus, pdev->dev, pdev->func, offset);
        pci_write_dword(pdev->bus, pdev->dev, pdev->func, offset, orig);
        
        if (sized == 0) {
            pdev->bar[i].size = 0;
            continue;
        }
        
        if (orig & 1) {
            //I/O space BAR
            pdev->bar[i].is_io = true;
            pdev->bar[i].is_64bit = false;
            pdev->bar[i].addr = orig & 0xFFFFFFFC;
            pdev->bar[i].size = ~(sized & 0xFFFFFFFC) + 1;
        } else {
            //memory space BAR
            pdev->bar[i].is_io = false;
            uint8 type = (orig >> 1) & 0x3;
            
            if (type == 0x2 && i < 5) {
                //64-bit BAR - combine with next
                uint8 next_off = 0x10 + ((i + 1) * 4);
                uint32 orig_hi = pci_read_dword(pdev->bus, pdev->dev, pdev->func, next_off);
                
                pci_write_dword(pdev->bus, pdev->dev, pdev->func, next_off, 0xFFFFFFFF);
                uint32 sized_hi = pci_read_dword(pdev->bus, pdev->dev, pdev->func, next_off);
                pci_write_dword(pdev->bus, pdev->dev, pdev->func, next_off, orig_hi);
                
                uint64 full_orig = ((uint64)orig_hi << 32) | orig;
                uint64 full_sized = ((uint64)sized_hi << 32) | sized;
                
                pdev->bar[i].is_64bit = true;
                pdev->bar[i].addr = full_orig & ~0xFULL;
                pdev->bar[i].size = ~(full_sized & ~0xFULL) + 1;
                
                //mark next BAR as consumed
                i++;
                pdev->bar[i].addr = 0;
                pdev->bar[i].size = 0;
                pdev->bar[i].is_io = false;
                pdev->bar[i].is_64bit = false;
            } else {
                //32-bit BAR
                pdev->bar[i].is_64bit = false;
                pdev->bar[i].addr = orig & 0xFFFFFFF0;
                pdev->bar[i].size = ~(sized & 0xFFFFFFF0) + 1;
            }
        }
    }
}

//device object ioctl handler
static int pci_dev_ioctl(object_t *obj, uint32 cmd, void *arg) {
    pci_device_t *pdev = (pci_device_t *)obj->data;
    if (!pdev) return -1;
    
    switch (cmd) {
        case PCI_IOCTL_GET_INFO: {
            if (!arg) return -1;
            memcpy(arg, pdev, sizeof(pci_device_t));
            return 0;
        }
        case PCI_IOCTL_CONFIG_READ: {
            pci_config_arg_t *ca = (pci_config_arg_t *)arg;
            if (!ca) return -1;
            ca->value = pci_config_read(pdev->bus, pdev->dev, pdev->func, ca->offset, ca->size);
            return 0;
        }
        case PCI_IOCTL_CONFIG_WRITE: {
            pci_config_arg_t *ca = (pci_config_arg_t *)arg;
            if (!ca) return -1;
            pci_config_write(pdev->bus, pdev->dev, pdev->func, ca->offset, ca->size, ca->value);
            return 0;
        }
        default:
            return -1;
    }
}

static object_ops_t pci_device_ops = {
    .read = NULL,
    .write = NULL,
    .close = NULL,
    .ioctl = pci_dev_ioctl,
    .readdir = NULL
};

//add device to list and register in namespace
static void pci_register_device(pci_device_t *pdev) {
    //add to linked list
    pdev->next = device_list;
    device_list = pdev;
    device_count++;
    
//create object and register in namespace
    object_t *obj = object_create(OBJECT_DEVICE, &pci_device_ops, pdev);
    if (obj) {
        char name[32];
        //format: $devices/pci/BB:DD.F
        snprintf(name, sizeof(name), "$devices/pci/%02x:%02x.%x", 
                 pdev->bus, pdev->dev, pdev->func);
        ns_register(name, obj);
        object_deref(obj);  //namespace holds the ref now
    }
    
    //log discovery
    printf("[pci] %02x:%02x.%x %04x:%04x %s (class %02x/%02x)\n",
           pdev->bus, pdev->dev, pdev->func,
           pdev->vendor_id, pdev->device_id,
           pci_class_name(pdev->class_code),
           pdev->class_code, pdev->subclass);
}

//probe a single device/function
static void pci_probe_function(uint8 bus, uint8 dev, uint8 func) {
    //cache first 64 bytes (16 dwords) of config space
    uint32 cfg[16];
    for (int i = 0; i < 16; i++) {
        cfg[i] = pci_read_dword(bus, dev, func, i * 4);
    }
    
    uint16 vendor = cfg[0] & 0xFFFF;
    if (vendor == 0xFFFF) return;  //no device
    
    pci_device_t *pdev = kzalloc(sizeof(pci_device_t));
    if (!pdev) return;
    
    pdev->vendor_id = vendor;
    pdev->device_id = (cfg[0] >> 16) & 0xFFFF;
    pdev->bus = bus;
    pdev->dev = dev;
    pdev->func = func;
    
    pdev->command = cfg[1] & 0xFFFF;
    pdev->status = (cfg[1] >> 16) & 0xFFFF;
    
    pdev->class_code = (cfg[2] >> 24) & 0xFF;
    pdev->subclass = (cfg[2] >> 16) & 0xFF;
    pdev->prog_if = (cfg[2] >> 8) & 0xFF;
    
    pdev->header_type = (cfg[3] >> 16) & 0xFF;
    
    //probe BARs for standard devices (header type 0)
    if ((pdev->header_type & PCI_HEADER_TYPE_MASK) == 0x00) {
        pdev->int_line = cfg[15] & 0xFF;        //offset 0x3C
        pdev->int_pin = (cfg[15] >> 8) & 0xFF;
        pci_probe_bars(pdev);
    }
    
    pci_register_device(pdev);
    
    //if this is a PCI-to-PCI bridge, scan the secondary bus
    if (pdev->class_code == PCI_CLASS_BRIDGE && 
        pdev->subclass == PCI_SUBCLASS_PCI2PCI) {
        uint8 secondary_bus = (cfg[6] >> 8) & 0xFF;  //offset 0x18
        if (secondary_bus != 0) {
            pci_scan_bus(secondary_bus);
        }
    }
}

//scan a single bus
static void pci_scan_bus(uint8 bus) {
    for (uint8 dev = 0; dev < 32; dev++) {
        uint32 reg0 = pci_read_dword(bus, dev, 0, 0x00);
        if ((reg0 & 0xFFFF) == 0xFFFF) continue;  //no device
        
        //probe function 0
        pci_probe_function(bus, dev, 0);
        
        //check if multifunction device
        uint32 reg3 = pci_read_dword(bus, dev, 0, 0x0C);
        if (reg3 & (PCI_HEADER_MULTIFUNCTION << 16)) {
            for (uint8 func = 1; func < 8; func++) {
                pci_probe_function(bus, dev, func);
            }
        }
    }
}

//public API implementations
void pci_init(void) {
    printf("[pci] Enumerating PCI devices...\n");
    
    //check if host bridge is multifunction (multiple root buses)
    uint32 reg3 = pci_read_dword(0, 0, 0, 0x0C);
    if (reg3 & (PCI_HEADER_MULTIFUNCTION << 16)) {
        //multiple PCI host controllers
        for (uint8 func = 0; func < 8; func++) {
            uint32 reg0 = pci_read_dword(0, 0, func, 0x00);
            if ((reg0 & 0xFFFF) != 0xFFFF) {
                pci_scan_bus(func);
            }
        }
    } else {
        //single root bus
        pci_scan_bus(0);
    }
    
    printf("[pci] Found %d device(s)\n", device_count);
}

pci_device_t *pci_find_device(uint16 vendor_id, uint16 device_id) {
    for (pci_device_t *d = device_list; d; d = d->next) {
        if (d->vendor_id == vendor_id && d->device_id == device_id) {
            return d;
        }
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8 class_code, uint8 subclass) {
    for (pci_device_t *d = device_list; d; d = d->next) {
        if (d->class_code == class_code && d->subclass == subclass) {
            return d;
        }
    }
    return NULL;
}

pci_device_t *pci_get_device(uint8 bus, uint8 dev, uint8 func) {
    for (pci_device_t *d = device_list; d; d = d->next) {
        if (d->bus == bus && d->dev == dev && d->func == func) {
            return d;
        }
    }
    return NULL;
}

pci_device_t *pci_get_devices(void) {
    return device_list;
}

uint32 pci_device_count(void) {
    return device_count;
}

void pci_enable_bus_master(pci_device_t *dev) {
    if (!dev) return;
    uint16 cmd = pci_config_read(dev->bus, dev->dev, dev->func, 0x04, 2);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write(dev->bus, dev->dev, dev->func, 0x04, 2, cmd);
    dev->command = cmd;
}

void pci_enable_mmio(pci_device_t *dev) {
    if (!dev) return;
    uint16 cmd = pci_config_read(dev->bus, dev->dev, dev->func, 0x04, 2);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_config_write(dev->bus, dev->dev, dev->func, 0x04, 2, cmd);
    dev->command = cmd;
}

void pci_enable_io(pci_device_t *dev) {
    if (!dev) return;
    uint16 cmd = pci_config_read(dev->bus, dev->dev, dev->func, 0x04, 2);
    cmd |= PCI_CMD_IO_SPACE;
    pci_config_write(dev->bus, dev->dev, dev->func, 0x04, 2, cmd);
    dev->command = cmd;
}