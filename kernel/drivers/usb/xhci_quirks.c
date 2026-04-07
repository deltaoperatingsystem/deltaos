#include <drivers/usb/xhci.h>
#include <lib/io.h>

#define XHCI_QUIRK_WILDCARD_DEVICE 0xFFFF

typedef struct {
    uint16 vendor_id;
    uint16 device_id;
    uint64 quirks;
    const char *label;
} xhci_quirk_match_t;

static const xhci_quirk_match_t xhci_quirk_matches[] = {
    //Renesas / NEC
    { 0x1033, XHCI_QUIRK_WILDCARD_DEVICE,
      XHCI_QUIRK_NEC_HOST,
      "Renesas/NEC host" },
    { 0x1033, 0x0014,
      XHCI_QUIRK_TRUST_TX_LENGTH | XHCI_QUIRK_ZERO_64B_REGS,
      "Renesas 0014 TX-length / 64-byte register quirk" },
    { 0x1033, 0x0015,
      XHCI_QUIRK_RESET_ON_RESUME | XHCI_QUIRK_ZERO_64B_REGS,
      "Renesas 0015 reset-on-resume / 64-byte register quirk" },
    { 0x1033, 0x0194,
      XHCI_QUIRK_PORT_POLLING_RECOVER |
      XHCI_QUIRK_PORT_POLLING_WARM_RESET |
      XHCI_QUIRK_FORCE_BIOS_HANDOFF |
      XHCI_QUIRK_RENESAS_FW_LOAD,
      "Renesas firmware / polling recovery" },

    //Fresco Logic
    { 0x1B73, 0x1000,
      XHCI_QUIRK_RESET_EP_QUIRK |
      XHCI_QUIRK_BROKEN_STREAMS |
      XHCI_QUIRK_BROKEN_MSI,
      "Fresco PDK reset-endpoint / MSI quirk" },
    { 0x1B73, 0x1009,
      XHCI_QUIRK_BROKEN_STREAMS,
      "Fresco FL1009 broken streams" },
    { 0x1B73, 0x1100,
      XHCI_QUIRK_TRUST_TX_LENGTH,
      "Fresco FL1100 TX-length quirk" },

    //VIA
    { 0x1106, 0x3432,
      XHCI_QUIRK_BROKEN_STREAMS,
      "VIA 3432 broken streams" },
    { 0x1106, 0x3483,
      XHCI_QUIRK_RESET_ON_RESUME |
      XHCI_QUIRK_TRB_OVERFETCH |
      XHCI_QUIRK_LPM_SUPPORT,
      "VIA VL805 reset-on-resume / TRB overfetch" },

    //Etron
    { 0x1B6F, 0x7023,
      XHCI_QUIRK_ETRON_HOST |
      XHCI_QUIRK_RESET_ON_RESUME |
      XHCI_QUIRK_BROKEN_STREAMS |
      XHCI_QUIRK_NO_SOFT_RETRY,
      "Etron EJ168 host quirks" },
    { 0x1B6F, 0x7052,
      XHCI_QUIRK_ETRON_HOST |
      XHCI_QUIRK_RESET_ON_RESUME |
      XHCI_QUIRK_BROKEN_STREAMS |
      XHCI_QUIRK_NO_SOFT_RETRY,
      "Etron EJ188 host quirks" },

    //ASMedia
    { 0x1B21, 0x1042,
      XHCI_QUIRK_SPURIOUS_SUCCESS |
      XHCI_QUIRK_BROKEN_STREAMS,
      "ASMedia 1042 spurious-success / broken-streams" },
    { 0x1B21, 0x1142,
      XHCI_QUIRK_NO_64BIT_SUPPORT |
      XHCI_QUIRK_ASMEDIA_MODIFY_FLOWCONTROL,
      "ASMedia 1142 no-64bit / flowcontrol" },
    { 0x1B21, 0x1242,
      XHCI_QUIRK_NO_64BIT_SUPPORT,
      "ASMedia 1242 no-64bit" },
    { 0x1B21, 0x2142,
      XHCI_QUIRK_NO_64BIT_SUPPORT,
      "ASMedia 2142 no-64bit" },
    { 0x1B21, 0x3042,
      XHCI_QUIRK_NO_64BIT_SUPPORT |
      XHCI_QUIRK_RESET_ON_RESUME,
      "ASMedia 3042 reset-on-resume" },
    { 0x1B21, 0x3242,
      XHCI_QUIRK_NO_64BIT_SUPPORT,
      "ASMedia 3242 no-64bit" },

    //AMD / ATI / Promontory family
    { 0x1022, 0x13ED,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9,
      "AMD Ariel Type-C interval clamp" },
    { 0x1022, 0x13EE,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9,
      "AMD Ariel Type-A interval clamp" },
    { 0x1022, 0x148C,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9,
      "AMD Starship interval clamp" },
    { 0x1022, 0x15D4,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9,
      "AMD Fireflight interval clamp" },
    { 0x1022, 0x15D5,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9,
      "AMD Fireflight interval clamp" },
    { 0x1022, 0x15E0,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9 |
      XHCI_QUIRK_SNPS_BROKEN_SUSPEND,
      "AMD Raven suspend / interval quirk" },
    { 0x1022, 0x15E1,
      XHCI_QUIRK_LIMIT_ENDPOINT_INTERVAL_9 |
      XHCI_QUIRK_SNPS_BROKEN_SUSPEND,
      "AMD Raven2 suspend / interval quirk" },
    { 0x1022, 0x15E5,
      XHCI_QUIRK_NO_SOFT_RETRY |
      XHCI_QUIRK_RESET_ON_RESUME,
      "AMD Raven2 no-soft-retry / reset-on-resume" },
    { 0x1022, 0x1639,
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "AMD Renoir runtime-PM allowance" },
    { 0x1022, 0x43B9,
      XHCI_QUIRK_NO_SOFT_RETRY,
      "AMD Promontory A4 no-soft-retry" },
    { 0x1022, 0x43BA,
      XHCI_QUIRK_NO_SOFT_RETRY,
      "AMD Promontory A3 no-soft-retry" },
    { 0x1022, 0x43BB,
      XHCI_QUIRK_NO_SOFT_RETRY,
      "AMD Promontory A2 no-soft-retry" },
    { 0x1022, 0x43BC,
      XHCI_QUIRK_NO_SOFT_RETRY,
      "AMD Promontory A1 no-soft-retry" },

    //Intel / Thunderbolt class controllers
    { 0x8086, 0x8C31,
      XHCI_QUIRK_PANTHERPOINT |
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Lynx Point host" },
    { 0x8086, 0x9C31,
      XHCI_QUIRK_PANTHERPOINT |
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Lynx Point LP host" },
    { 0x8086, 0x9CB1,
      XHCI_QUIRK_PANTHERPOINT |
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Wildcat Point LP host" },
    { 0x8086, 0x22B5,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Cherryview host" },
    { 0x8086, 0xA12F,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Sunrise Point H host" },
    { 0x8086, 0x9D2F,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Sunrise Point LP host" },
    { 0x8086, 0x0AA8,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Broxton M host" },
    { 0x8086, 0x1AA8,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Broxton B host" },
    { 0x8086, 0x5AA8,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Apollo Lake host" },
    { 0x8086, 0x19D0,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Denverton host" },
    { 0x8086, 0x8A13,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Ice Lake host" },
    { 0x8086, 0x9A13,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Tiger Lake host" },
    { 0x8086, 0xA0ED,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Tiger Lake PCH host" },
    { 0x8086, 0xA3AF,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Comet Lake host" },
    { 0x8086, 0x51ED,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alder Lake PCH host" },
    { 0x8086, 0x54ED,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alder Lake N PCH host" },
    { 0x8086, 0x1138,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Maple Ridge host" },
    { 0x8086, 0x15B5,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alpine Ridge 2C host" },
    { 0x8086, 0x15B6,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alpine Ridge 4C host" },
    { 0x8086, 0x15C1,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alpine Ridge LP host" },
    { 0x8086, 0x15DB,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alpine Ridge C 2C host" },
    { 0x8086, 0x15D4,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Alpine Ridge C 4C host" },
    { 0x8086, 0x15E9,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Titan Ridge 2C host" },
    { 0x8086, 0x15EC,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Titan Ridge 4C host" },
    { 0x8086, 0x15F0,
      XHCI_QUIRK_INTEL_HOST |
      XHCI_QUIRK_DEFAULT_PM_RUNTIME_ALLOW,
      "Intel Titan Ridge DD host" },
};

void xhci_apply_pci_quirks(xhci_ctrl_t *c, pci_device_t *pci) {
    if (!c || !pci) return;

    for (size i = 0; i < sizeof(xhci_quirk_matches) / sizeof(xhci_quirk_matches[0]); i++) {
        const xhci_quirk_match_t *match = &xhci_quirk_matches[i];
        if (match->vendor_id != pci->vendor_id) continue;
        if (match->device_id != XHCI_QUIRK_WILDCARD_DEVICE &&
            match->device_id != pci->device_id) {
            continue;
        }

        c->quirks |= match->quirks;
        printf("[xhci] quirk: %s (%04x:%04x)\n",
               match->label, pci->vendor_id, pci->device_id);
    }

    if (pci->vendor_id == 0x1033 && pci->device_id == 0x0194) {
        //Renesas firmware loader is handled separately so it can stay isolated
        //from the generic quirk table and keep the big vendor-specific path
        //out of xhci.c.
        c->quirks |= XHCI_QUIRK_RENESAS_FW_LOAD;
    }
}
