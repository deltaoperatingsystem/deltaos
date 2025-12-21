#include <kernel/device.h>
#include <mm/kheap.h>
#include <lib/io.h>

#define DEVICE_INITIAL_CAP 8
#define DEVICE_GROW_FACTOR 2

static struct device **devices = NULL;
static uint32 device_capacity = 0;
static uint32 device_num = 0;

static bool streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a++ != *b++) return false;
    }
    return *a == *b;
}

void device_init(void) {
    devices = kmalloc(DEVICE_INITIAL_CAP * sizeof(struct device *));
    if (!devices) {
        printf("[device] ERR: failed to allocate device array\n");
        return;
    }
    device_capacity = DEVICE_INITIAL_CAP;
    device_num = 0;
}

void device_register(struct device *dev) {
    if (!dev || !devices) return;
    
    //grow array if needed
    if (device_num >= device_capacity) {
        uint32 new_cap = device_capacity * DEVICE_GROW_FACTOR;
        struct device **new_arr = krealloc(devices, new_cap * sizeof(struct device *));
        if (!new_arr) {
            printf("[device] ERR: failed to grow device array\n");
            return;
        }
        devices = new_arr;
        device_capacity = new_cap;
    }
    
    devices[device_num++] = dev;
}

struct device *device_find(const char *name) {
    if (!name || !devices) return NULL;
    
    for (uint32 i = 0; i < device_num; i++) {
        if (devices[i] && streq(devices[i]->name, name)) {
            return devices[i];
        }
    }
    return NULL;
}

struct device *device_find_type(enum device_type type) {
    if (!devices) return NULL;
    for (uint32 i = 0; i < device_num; i++) {
        if (devices[i] && devices[i]->type == type) {
            return devices[i];
        }
    }
    return NULL;
}

struct device *device_find_subtype(enum device_subtype subtype) {
    if (!devices) return NULL;
    for (uint32 i = 0; i < device_num; i++) {
        if (devices[i] && devices[i]->subtype == subtype) {
            return devices[i];
        }
    }
    return NULL;
}

struct device *device_next(struct device *prev) {
    if (!devices) return NULL;
    if (!prev) {
        return device_num > 0 ? devices[0] : NULL;
    }
    
    for (uint32 i = 0; i < device_num; i++) {
        if (devices[i] == prev && i + 1 < device_num) {
            return devices[i + 1];
        }
    }
    return NULL;
}

uint32 device_count(void) {
    return device_num;
}
