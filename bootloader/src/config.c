#include "config.h"
#include <string.h>

static void skip_whitespace(const char **p, const char *end) {
    while (*p < end && is_whitespace(**p)) (*p)++;
}

static void skip_line(const char **p, const char *end) {
    while (*p < end && **p != '\n') (*p)++;
    if (*p < end) (*p)++;
}
static int parse_int(const char **p, const char *end) {
    int val = 0;
    while (*p < end && is_digit(**p)) {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

static void copy_until(const char **p, const char *end, char *dest, int max, char stop) {
    int i = 0;
    while (*p < end && **p != stop && **p != '\n' && **p != '\r' && i < max - 1) {
        dest[i++] = **p;
        (*p)++;
    }
    dest[i] = 0;
    //trim trailing whitespace
    while (i > 0 && is_whitespace(dest[i-1])) {
        dest[--i] = 0;
    }
}

int config_parse(const char *data, uint64_t size, Config *cfg) {
    const char *p = data;
    const char *end = data + size;
    ConfigEntry *current = 0;
    
    //defaults
    cfg->timeout = 0;
    cfg->default_entry = 0;
    cfg->entry_count = 0;
    
    while (p < end) {
        skip_whitespace(&p, end);
        if (p >= end) break;
        
        //comment
        if (*p == '#' || *p == ';') {
            skip_line(&p, end);
            continue;
        }
        
        //section header [name]
        if (*p == '[') {
            p++;
            if (cfg->entry_count >= CONFIG_MAX_ENTRIES) {
                skip_line(&p, end);
                continue;
            }
            current = &cfg->entries[cfg->entry_count++];
            current->name[0] = 0;
            current->path[0] = 0;
            current->cmdline[0] = 0;
            current->initrd[0] = 0;
            copy_until(&p, end, current->name, sizeof(current->name), ']');
            skip_line(&p, end);
            continue;
        }
        
        //key=value
        if (str_starts_with(p, "timeout=")) {
            p += 8;
            cfg->timeout = parse_int(&p, end);
            skip_line(&p, end);
            continue;
        }
        
        if (str_starts_with(p, "default=")) {
            p += 8;
            cfg->default_entry = parse_int(&p, end);
            skip_line(&p, end);
            continue;
        }
        
        if (current) {
            if (str_starts_with(p, "path=")) {
                p += 5;
                copy_until(&p, end, current->path, sizeof(current->path), '\n');
                skip_line(&p, end);
                continue;
            }
            
            if (str_starts_with(p, "cmdline=")) {
                p += 8;
                copy_until(&p, end, current->cmdline, sizeof(current->cmdline), '\n');
                skip_line(&p, end);
                continue;
            }
            if (str_starts_with(p, "initrd=")) {
                p += 7;
                copy_until(&p, end, current->initrd, sizeof(current->initrd), '\n');
                skip_line(&p, end);
                continue;
            }
        }
        
        //unknown line so skip
        skip_line(&p, end);
    }
    
    return cfg->entry_count;
}

ConfigEntry *config_get_entry(Config *cfg, int index) {
    if (index < 0 || (uint32_t)index >= cfg->entry_count) return 0;
    return &cfg->entries[index];
}
