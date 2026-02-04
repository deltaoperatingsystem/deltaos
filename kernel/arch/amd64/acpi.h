#ifndef ARCH_AMD64_ACPI_H
#define ARCH_AMD64_ACPI_H

#include <arch/amd64/types.h>

typedef struct acpi_header {
    char signature[4];
    uint32 length;
    uint8 revision;
    uint8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32 oem_revision;
    uint32 creator_id;
    uint32 creator_revision;
} __attribute__((packed)) acpi_header_t;

typedef struct acpi_rsdp {
    char signature[8];
    uint8 checksum;
    char oem_id[6];
    uint8 revision;
    uint32 rsdt_address;
    
    //ACPI 2.0+
    uint32 length;
    uint64 xsdt_address;
    uint8 extended_checksum;
    uint8 reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct acpi_rsdt {
    acpi_header_t header;
    uint32 tables[];
} __attribute__((packed)) acpi_rsdt_t;

typedef struct acpi_xsdt {
    acpi_header_t header;
    uint64 tables[];
} __attribute__((packed)) acpi_xsdt_t;

//MADT
#define ACPI_MADT_SIGNATURE "APIC"

typedef struct acpi_madt {
    acpi_header_t header;
    uint32 local_apic_address;
    uint32 flags;
    uint8 entries[];
} __attribute__((packed)) acpi_madt_t;

//MADT Entry Types
#define ACPI_MADT_TYPE_LOCAL_APIC           0
#define ACPI_MADT_TYPE_IO_APIC              1
#define ACPI_MADT_TYPE_INT_SRC_OVERRIDE     2
#define ACPI_MADT_TYPE_NMI_INT_SRC          3
#define ACPI_MADT_TYPE_LOCAL_APIC_NMI       4
#define ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE  5

typedef struct acpi_madt_entry {
    uint8 type;
    uint8 length;
} __attribute__((packed)) acpi_madt_entry_t;

typedef struct acpi_madt_local_apic {
    acpi_madt_entry_t header;
    uint8 processor_id;
    uint8 apic_id;
    uint32 flags; //bit 0 = enabled
} __attribute__((packed)) acpi_madt_local_apic_t;

typedef struct acpi_madt_io_apic {
    acpi_madt_entry_t header;
    uint8 io_apic_id;
    uint8 reserved;
    uint32 io_apic_address;
    uint32 global_system_interrupt_base;
} __attribute__((packed)) acpi_madt_io_apic_t;

typedef struct acpi_madt_int_src_override {
    acpi_madt_entry_t header;
    uint8 bus_source; //always 0 for ISA
    uint8 irq_source;
    uint32 global_system_interrupt;
    uint16 flags;
} __attribute__((packed)) acpi_madt_int_src_override_t;

//MCFG
#define ACPI_MCFG_SIGNATURE "MCFG"

typedef struct acpi_mcfg_entry {
    uint64 base_address;
    uint16 pci_segment_group;
    uint8 start_bus;
    uint8 end_bus;
    uint32 reserved;
} __attribute__((packed)) acpi_mcfg_entry_t;

typedef struct acpi_mcfg {
    acpi_header_t header;
    uint64 reserved;
    acpi_mcfg_entry_t entries[];
} __attribute__((packed)) acpi_mcfg_t;

void acpi_init(void);
void *acpi_find_table(const char *signature);

//system configuration derived from ACPI
extern uint32 acpi_cpu_count;
extern uint8 acpi_cpu_ids[64];
extern uint32 acpi_ioapic_addr;
extern uint32 acpi_lapic_addr;

typedef struct acpi_iso {
    uint8 irq_source;
    uint32 gsi;
    uint16 flags;
} acpi_iso_t;

extern acpi_iso_t acpi_isos[16];
extern uint32 acpi_iso_count;

extern uint64 acpi_mcfg_addr;
extern uint8 acpi_mcfg_start_bus;
extern uint8 acpi_mcfg_end_bus;

#endif
