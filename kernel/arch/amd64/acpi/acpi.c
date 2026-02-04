#include <arch/amd64/acpi/acpi.h>
#include <boot/db.h>
#include <mm/mm.h>
#include <lib/string.h>
#include <lib/io.h>
#include <drivers/serial.h>

static void *rsdp_virt = NULL;
static acpi_rsdt_t *rsdt = NULL;
static acpi_xsdt_t *xsdt = NULL;
static bool use_xsdt = false;

static bool acpi_checksum(acpi_header_t *header) {
    uint8 sum = 0;
    for (uint32 i = 0; i < header->length; i++) {
        sum += ((uint8 *)header)[i];
    }
    return sum == 0;
}

void *acpi_find_table(const char *signature) {
    if (!rsdt && !xsdt) return NULL;

    int entries = 0;
    if (use_xsdt) {
        entries = (xsdt->header.length - sizeof(acpi_header_t)) / sizeof(uint64);
    } else {
        entries = (rsdt->header.length - sizeof(acpi_header_t)) / sizeof(uint32);
    }

    for (int i = 0; i < entries; i++) {
        acpi_header_t *header;
        if (use_xsdt) {
            header = (acpi_header_t *)P2V(xsdt->tables[i]);
        } else {
            header = (acpi_header_t *)P2V((uintptr)rsdt->tables[i]);
        }

        if (strncmp(header->signature, signature, 4) == 0) {
            if (acpi_checksum(header)) {
                return header;
            } else {
                serial_write("[acpi] ERR: table checksum failed: ");
                serial_write(signature);
                serial_write("\n");
            }
        }
    }

    return NULL;
}

uint32 acpi_cpu_count = 0;
uint8 acpi_cpu_ids[64];
uint32 acpi_ioapic_addr = 0;
uint32 acpi_lapic_addr = 0;
acpi_iso_t acpi_isos[16];
uint32 acpi_iso_count = 0;

uint64 acpi_mcfg_addr = 0;
uint8 acpi_mcfg_start_bus = 0;
uint8 acpi_mcfg_end_bus = 0;

static void acpi_parse_mcfg(acpi_mcfg_t *mcfg) {
    int entries = (mcfg->header.length - sizeof(acpi_mcfg_t)) / sizeof(acpi_mcfg_entry_t);
    if (entries > 0) {
        //we currently only supbport one ECAM segment
        acpi_mcfg_addr = mcfg->entries[0].base_address;
        acpi_mcfg_start_bus = mcfg->entries[0].start_bus;
        acpi_mcfg_end_bus = mcfg->entries[0].end_bus;
        printf("[acpi] MCFG: ECAM at 0x%lx (Buses %u-%u)\n", 
               acpi_mcfg_addr, acpi_mcfg_start_bus, acpi_mcfg_end_bus);
    }
}

static void acpi_parse_madt(acpi_madt_t *madt) {
    acpi_lapic_addr = madt->local_apic_address;
    
    uint8 *p = madt->entries;
    uint8 *end = (uint8 *)madt + madt->header.length;
    
    while (p < end) {
        acpi_madt_entry_t *entry = (acpi_madt_entry_t *)p;
        if (entry->length == 0) break;

        switch (entry->type) {
            case ACPI_MADT_TYPE_LOCAL_APIC: {
                acpi_madt_local_apic_t *lapic = (acpi_madt_local_apic_t *)entry;
                if (lapic->flags & 1) { //enabled
                    if (acpi_cpu_count < 64) {
                        acpi_cpu_ids[acpi_cpu_count++] = lapic->apic_id;
                    }
                }
                break;
            }
            case ACPI_MADT_TYPE_IO_APIC: {
                acpi_madt_io_apic_t *ioapic = (acpi_madt_io_apic_t *)entry;
                acpi_ioapic_addr = ioapic->io_apic_address;
                break;
            }
            case ACPI_MADT_TYPE_INT_SRC_OVERRIDE: {
                acpi_madt_int_src_override_t *iso = (acpi_madt_int_src_override_t *)entry;
                if (acpi_iso_count < 16) {
                    acpi_isos[acpi_iso_count].irq_source = iso->irq_source;
                    acpi_isos[acpi_iso_count].gsi = iso->global_system_interrupt;
                    acpi_isos[acpi_iso_count].flags = iso->flags;
                    acpi_iso_count++;
                }
                break;
            }
        }
        p += entry->length;
    }

    printf("[acpi] MADT: %u CPUs, Local APIC @ 0x%x, IO APIC @ 0x%x\n", 
           acpi_cpu_count, acpi_lapic_addr, acpi_ioapic_addr);
}

void acpi_init(void) {
    struct db_tag_acpi_rsdp *tag = db_get_acpi_rsdp();
    if (!tag) {
        serial_write("[acpi] ERR: No RSDP tag found in boot info\n");
        return;
    }

    rsdp_virt = P2V(tag->rsdp_address);
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)rsdp_virt;

    if (strncmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        serial_write("[acpi] ERR: Invalid RSDP signature\n");
        return;
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        xsdt = (acpi_xsdt_t *)P2V(rsdp->xsdt_address);
        use_xsdt = true;
        serial_write("[acpi] Using XSDP/XSDT\n");
    } else {
        rsdt = (acpi_rsdt_t *)P2V((uintptr)rsdp->rsdt_address);
        use_xsdt = false;
        serial_write("[acpi] Using RSDP/RSDT\n");
    }

    acpi_header_t *main_header = use_xsdt ? &xsdt->header : &rsdt->header;
    if (!acpi_checksum(main_header)) {
        serial_write("[acpi] ERR: RSDT/XSDT checksum failed\n");
        rsdt = NULL;
        xsdt = NULL;
        return;
    }

    serial_write("[acpi] Initialized\n");

    acpi_madt_t *madt = acpi_find_table(ACPI_MADT_SIGNATURE);
    if (madt) {
        acpi_parse_madt(madt);
    } else {
        serial_write("[acpi] ERR: MADT not found\n");
    }

    acpi_mcfg_t *mcfg = acpi_find_table(ACPI_MCFG_SIGNATURE);
    if (mcfg) {
        acpi_parse_mcfg(mcfg);
    }
}
