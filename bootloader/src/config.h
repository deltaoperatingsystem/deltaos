#ifndef _CONFIG_H
#define _CONFIG_H

#include <stdint.h>

#define CONFIG_MAX_ENTRIES  16
#define CONFIG_MAX_PATH     256
#define CONFIG_MAX_CMDLINE  512

typedef struct {
    char name[64];
    char path[CONFIG_MAX_PATH];
    char cmdline[CONFIG_MAX_CMDLINE];
    char initrd[CONFIG_MAX_PATH];
} ConfigEntry;

typedef struct {
    uint32_t timeout;           //auto-boot timeout (0 = disabled)
    uint32_t default_entry;     //default boot entry index
    uint32_t entry_count;
    ConfigEntry entries[CONFIG_MAX_ENTRIES];
} Config;

int config_parse(const char *data, uint64_t size, Config *cfg);
ConfigEntry *config_get_entry(Config *cfg, int index);

#endif
