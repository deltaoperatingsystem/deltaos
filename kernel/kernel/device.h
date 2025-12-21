#ifndef KERNEL_DEVICE_H
#define KERNEL_DEVICE_H

#include <arch/types.h>

//device types
enum device_type {
    DEV_CHAR,        //character device
    DEV_BLOCK,       //block device
    DEV_NET,         //network device
    DEV_DISPLAY,     //display/framebuffer
    DEV_INPUT,       //input device
    DEV_SOUND,       //audio device
    DEV_BUS,         //bus controller
    DEV_TIMER,       //timer device
    DEV_TIME,        //time/date device
    DEV_OTHER        //misc
};

//device sub-types
enum device_subtype {
    //char devices
    SUBDEV_SERIAL,
    SUBDEV_CONSOLE,
    SUBDEV_PTY,
    
    //block devices
    SUBDEV_DISK,
    SUBDEV_PARTITION,
    SUBDEV_RAMDISK,
    
    //network devices
    SUBDEV_ETHERNET,
    SUBDEV_LOOPBACK,
    
    //display devices
    SUBDEV_FRAMEBUFFER,
    SUBDEV_GPU,
    
    //input devices
    SUBDEV_KEYBOARD,
    SUBDEV_MOUSE,
    SUBDEV_TOUCHPAD,
    
    //bus types
    SUBDEV_PCI,
    SUBDEV_USB,
    SUBDEV_I2C,

    //time devices
    SUBDEV_RTC,
    
    SUBDEV_NONE = 0xFF
};

struct device;

//operations each device can implement
struct device_ops {
    int (*open)(struct device *dev);
    int (*close)(struct device *dev);
    ssize (*read)(struct device *dev, void *buf, size len);
    ssize (*write)(struct device *dev, const void *buf, size len);
    int (*ioctl)(struct device *dev, uint32 cmd, void *arg);
};

//device structure
struct device {
    const char *name;
    enum device_type type;
    enum device_subtype subtype;
    struct device_ops *ops;
    void *private;  //driver-specific data
};

//initialize the device manager (call after kheap_init)
void device_init(void);

//register a device with the manager
void device_register(struct device *dev);

//find device by name (returns NULL if not found)
struct device *device_find(const char *name);

//find first device of given type (returns NULL if not found)
struct device *device_find_type(enum device_type type);

//find first device of given subtype (returns NULL if not found)
struct device *device_find_subtype(enum device_subtype subtype);

//iterate all devices (returns NULL when done)
struct device *device_next(struct device *prev);

//get device count
uint32 device_count(void);

#endif
