#include <drivers/pci.h>
#include <drivers/pci_protocol.h>
#include <arch/pci.h>
#include <arch/io.h>
#include <mm/kheap.h>
#include <obj/object.h>
#include <obj/namespace.h>
#include <ipc/channel.h>
#include <ipc/channel_server.h>
#include <proc/process.h>
#include <lib/io.h>
#include <lib/string.h>
#include <drivers/init.h>

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

//raw config space read (dword aligned)
static uint32 pci_read_dword(uint8 bus, uint8 dev, uint8 func, uint16 offset) {
    return arch_pci_read(bus, dev, func, offset & ~3, 4);
}

//raw config space write (dword aligned)
static void pci_write_dword(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint32 value) {
    arch_pci_write(bus, dev, func, offset & ~3, 4, value);
}

//public config read with size support
uint32 pci_config_read(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size) {
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
void pci_config_write(uint8 bus, uint8 dev, uint8 func, uint16 offset, uint8 size, uint32 value) {
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

static object_ops_t pci_device_ops = {
    .read = NULL,
    .write = NULL,
    .close = NULL,
    .readdir = NULL,
    .lookup = NULL
};

//PCI channel message handler
static void pci_channel_handler(channel_endpoint_t *ep, channel_msg_t *msg, void *ctx) {
    pci_device_t *pdev = (pci_device_t *)ctx;
    if (!pdev || !msg || !msg->data || msg->data_len < sizeof(pci_msg_hdr_t)) {
        if (msg->data) kfree(msg->data);
        return;
    }
    
    pci_msg_hdr_t *hdr = (pci_msg_hdr_t *)msg->data;
    uint32 txn_id = hdr->txn_id;
    
    switch (hdr->type) {
        case PCI_MSG_GET_INFO: {
            pci_msg_info_resp_t resp = {0};
            resp.hdr.type = PCI_MSG_GET_INFO | PCI_MSG_RESPONSE;
            resp.hdr.txn_id = txn_id;
            resp.hdr.status = 0;
            resp.vendor_id = pdev->vendor_id;
            resp.device_id = pdev->device_id;
            resp.class_code = pdev->class_code;
            resp.subclass = pdev->subclass;
            resp.prog_if = pdev->prog_if;
            resp.header_type = pdev->header_type;
            resp.int_line = pdev->int_line;
            resp.int_pin = pdev->int_pin;
            resp.bus = pdev->bus;
            resp.dev = pdev->dev;
            resp.func = pdev->func;
            
            channel_msg_t reply = {
                .data = &resp,
                .data_len = sizeof(resp),
                .handles = NULL,
                .handle_count = 0
            };
            channel_reply(ep, &reply);
            break;
        }
        
        case PCI_MSG_CONFIG_READ: {
            if (msg->data_len < sizeof(pci_msg_config_read_req_t)) break;
            pci_msg_config_read_req_t *req = (pci_msg_config_read_req_t *)msg->data;
            
            pci_msg_config_read_resp_t resp = {0};
            resp.hdr.type = PCI_MSG_CONFIG_READ | PCI_MSG_RESPONSE;
            resp.hdr.txn_id = txn_id;
            resp.hdr.status = 0;
            resp.value = pci_config_read(pdev->bus, pdev->dev, pdev->func, 
                                         req->offset, req->size);
            
            channel_msg_t reply = {
                .data = &resp,
                .data_len = sizeof(resp),
                .handles = NULL,
                .handle_count = 0
            };
            channel_reply(ep, &reply);
            break;
        }
        
        case PCI_MSG_CONFIG_WRITE: {
            if (msg->data_len < sizeof(pci_msg_config_write_req_t)) break;
            pci_msg_config_write_req_t *req = (pci_msg_config_write_req_t *)msg->data;
            
            pci_config_write(pdev->bus, pdev->dev, pdev->func,
                            req->offset, req->size, req->value);
            
            pci_msg_hdr_t resp = {
                .type = PCI_MSG_CONFIG_WRITE | PCI_MSG_RESPONSE,
                .txn_id = txn_id,
                .status = 0
            };
            
            channel_msg_t reply = {
                .data = &resp,
                .data_len = sizeof(resp),
                .handles = NULL,
                .handle_count = 0
            };
            channel_reply(ep, &reply);
            break;
        }
        
        case PCI_MSG_GET_BAR: {
            if (msg->data_len < sizeof(pci_msg_get_bar_req_t)) break;
            pci_msg_get_bar_req_t *req = (pci_msg_get_bar_req_t *)msg->data;
            
            pci_msg_get_bar_resp_t resp = {0};
            resp.hdr.type = PCI_MSG_GET_BAR | PCI_MSG_RESPONSE;
            resp.hdr.txn_id = txn_id;
            
            if (req->bar_index < 6) {
                resp.hdr.status = 0;
                resp.addr = pdev->bar[req->bar_index].addr;
                resp.size = pdev->bar[req->bar_index].size;
                resp.is_io = pdev->bar[req->bar_index].is_io;
                resp.is_64bit = pdev->bar[req->bar_index].is_64bit;
            } else {
                resp.hdr.status = -1;  //invalid BAR index
            }
            
            channel_msg_t reply = {
                .data = &resp,
                .data_len = sizeof(resp),
                .handles = NULL,
                .handle_count = 0
            };
            channel_reply(ep, &reply);
            break;
        }
        
        default:
            //unknown message type - send error response
            {
                pci_msg_hdr_t resp = {
                    .type = hdr->type | PCI_MSG_RESPONSE,
                    .txn_id = txn_id,
                    .status = -1
                };
                channel_msg_t reply = {
                    .data = &resp,
                    .data_len = sizeof(resp),
                    .handles = NULL,
                    .handle_count = 0
                };
                channel_reply(ep, &reply);
            }
            break;
    }
    
    //free the request message
    kfree(msg->data);
}

//add device to list and register in namespace
static void pci_register_device(pci_device_t *pdev) {
    //add to linked list
    pdev->next = device_list;
    device_list = pdev;
    device_count++;
    
    //create object and register in namespace
    object_t *obj = object_create(OBJECT_DEVICE, &pci_device_ops, pdev);
    if (obj) {
        char name[48];
        //format: $devices/pci/BB:DD.F
        snprintf(name, sizeof(name), "$devices/pci/%02X:%02X.%X", 
                 pdev->bus, pdev->dev, pdev->func);
        ns_register(name, obj);
        object_deref(obj);  //namespace holds the ref now
    }
    
    //create a channel for this device
    process_t *kproc = process_get_kernel();
    if (kproc) {
        int32 client_ep, server_ep;
        if (channel_create(kproc, HANDLE_RIGHTS_DEFAULT, &client_ep, &server_ep) == 0) {
            //get the server endpoint object and register handler
            channel_endpoint_t *server = channel_get_endpoint(kproc, server_ep);
            if (server) {
                channel_set_handler(server, pci_channel_handler, pdev);
            }
            
            //register the client endpoint in namespace
            object_t *client_obj = process_get_handle(kproc, client_ep);
            if (client_obj) {
                char chan_name[48];
                snprintf(chan_name, sizeof(chan_name), "$devices/pci/%02X:%02X.%X/channel",
                         pdev->bus, pdev->dev, pdev->func);
                object_ref(client_obj);  //ns_register doesn't add ref
                ns_register(chan_name, client_obj);
            }
        }
    }
    
    //log discovery
    printf("[pci] %02X:%02X.%X %04X:%04X %s (class %02X/%02X PI %02X)\n",
           pdev->bus, pdev->dev, pdev->func,
           pdev->vendor_id, pdev->device_id,
           pci_class_name(pdev->class_code),
           pdev->class_code, pdev->subclass, pdev->prog_if);
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

DECLARE_DRIVER(pci_init, INIT_LEVEL_BUS);

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