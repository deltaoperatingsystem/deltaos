#include <arch/amd64/acpi/dmar.h>
#include <drivers/pci.h>
#include <lib/io.h>
#include <drivers/serial.h>

uint64 dmar_iommu_base = 0;
bool   dmar_ir_supported = false;
bool   dmar_x2apic_opt_out = false;
uint8  dmar_host_aw = 0;
bool   dmar_ioapic_sid_valid = false;
uint16 dmar_ioapic_sid = 0;

static bool dmar_resolve_scope_sid(dmar_dev_scope_t *scope, uint16 segment, uint16 *sid_out) {
    if (!scope || !sid_out) return false;
    if (segment != 0) return false;
    if (scope->length < sizeof(dmar_dev_scope_t)) return false;

    uint32 path_len = scope->length - sizeof(dmar_dev_scope_t);
    if (path_len < sizeof(dmar_pci_path_t) || (path_len % sizeof(dmar_pci_path_t)) != 0) {
        return false;
    }

    dmar_pci_path_t *path = (dmar_pci_path_t *)((uint8 *)scope + sizeof(dmar_dev_scope_t));
    uint32 path_count = path_len / sizeof(dmar_pci_path_t);
    uint8 bus = scope->start_bus;

    for (uint32 i = 0; i < path_count; i++) {
        uint8 dev = path[i].device;
        uint8 func = path[i].function;

        if (i + 1 == path_count) {
            *sid_out = ((uint16)bus << 8) | ((uint16)dev << 3) | (func & 0x7);
            return true;
        }

        uint32 vendor = pci_config_read(bus, dev, func, 0x00, 2);
        uint32 header = pci_config_read(bus, dev, func, 0x0E, 1);
        if (vendor == 0xFFFF || (header & 0x7F) != 0x01) {
            return false;
        }

        bus = pci_config_read(bus, dev, func, 0x19, 1);
        if (bus == 0 || bus == 0xFF) {
            return false;
        }
    }

    return false;
}

void dmar_init(void) {
    acpi_dmar_t *dmar = acpi_find_table(ACPI_DMAR_SIGNATURE);
    if (!dmar) {
        serial_write("[dmar] DMAR table not found\n");
        return;
    }

    dmar_host_aw = dmar->host_address_width + 1;
    dmar_ir_supported = (dmar->flags & DMAR_FLAG_INTR_REMAP) != 0;
    dmar_x2apic_opt_out = (dmar->flags & DMAR_FLAG_X2APIC_OPT_OUT) != 0;

    printf("[dmar] DMAR: host address width %u, flags 0x%02x (IR %s, x2APIC opt-out %s)\n",
           dmar_host_aw, dmar->flags,
           dmar_ir_supported ? "supported" : "not supported",
           dmar_x2apic_opt_out ? "set" : "clear");

    //walk remapping structures looking for DRHD
    uint8 *p = (uint8 *)dmar + sizeof(acpi_dmar_t);
    uint8 *end = (uint8 *)dmar + dmar->header.length;
    uint64 first_drhd_base = 0;
    uint64 include_all_drhd_base = 0;
    uint64 ioapic_drhd_base = 0;

    while (p + sizeof(dmar_remap_header_t) <= end) {
        dmar_remap_header_t *hdr = (dmar_remap_header_t *)p;
        if (hdr->length < sizeof(dmar_remap_header_t)) break;
        if (p + hdr->length > end) break;

        if (hdr->type == DMAR_TYPE_DRHD) {
            dmar_drhd_t *drhd = (dmar_drhd_t *)p;
            printf("[dmar] DRHD: segment %u, base 0x%lx, flags 0x%02x%s\n",
                   drhd->segment, drhd->register_base_address, drhd->flags,
                   (drhd->flags & DRHD_FLAG_INCLUDE_PCI_ALL) ? " (INCLUDE_PCI_ALL)" : "");

            if (!first_drhd_base)
                first_drhd_base = drhd->register_base_address;

            //prefer the catch-all DRHD
            if (drhd->flags & DRHD_FLAG_INCLUDE_PCI_ALL) {
                include_all_drhd_base = drhd->register_base_address;
            }

            uint8 *scope_ptr = p + sizeof(dmar_drhd_t);
            uint8 *scope_end = p + hdr->length;
            while (scope_ptr + sizeof(dmar_dev_scope_t) <= scope_end) {
                dmar_dev_scope_t *scope = (dmar_dev_scope_t *)scope_ptr;
                if (scope->length < sizeof(dmar_dev_scope_t) || scope_ptr + scope->length > scope_end) {
                    break;
                }

                if (scope->type == DMAR_SCOPE_IOAPIC) {
                    uint16 sid = 0;
                    bool sid_ok = dmar_resolve_scope_sid(scope, drhd->segment, &sid);
                    printf("[dmar]   scope: IOAPIC enum %u bus %u%s\n",
                           scope->enumeration_id, scope->start_bus,
                           sid_ok ? "" : " (SID unresolved)");

                    if (scope->enumeration_id == acpi_ioapic_id && sid_ok) {
                        dmar_ioapic_sid = sid;
                        dmar_ioapic_sid_valid = true;
                        ioapic_drhd_base = drhd->register_base_address;
                        printf("[dmar]   matched MADT IOAPIC id %u -> SID %02x:%02x.%u\n",
                               acpi_ioapic_id, sid >> 8, (sid >> 3) & 0x1F, sid & 0x7);
                    }
                }

                scope_ptr += scope->length;
            }
        }

        p += hdr->length;
    }

    if (ioapic_drhd_base) {
        dmar_iommu_base = ioapic_drhd_base;
        serial_write("[dmar] using IOAPIC-scoped DRHD for interrupt remapping\n");
    } else if (include_all_drhd_base) {
        dmar_iommu_base = include_all_drhd_base;
    } else if (first_drhd_base) {
        dmar_iommu_base = first_drhd_base;
        serial_write("[dmar] no INCLUDE_PCI_ALL DRHD, using first DRHD as fallback\n");
    }

    if (dmar_iommu_base && dmar_ir_supported) {
        printf("[dmar] using IOMMU at 0x%lx for interrupt remapping\n", dmar_iommu_base);
        if (dmar_ioapic_sid_valid) {
            printf("[dmar] IOAPIC source-id validation enabled with SID %02x:%02x.%u\n",
                   dmar_ioapic_sid >> 8, (dmar_ioapic_sid >> 3) & 0x1F, dmar_ioapic_sid & 0x7);
        } else {
            serial_write("[dmar] warning: no matching IOAPIC device scope, IOAPIC IRTEs will skip source validation\n");
        }
    } else if (!dmar_ir_supported) {
        serial_write("[dmar] firmware does not advertise interrupt remapping\n");
    } else {
        serial_write("[dmar] no usable DRHD found\n");
    }
}
