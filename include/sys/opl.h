#ifndef SYS_OPL_H
#define SYS_OPL_H

#include <sys/types.h>

#define OPL_CHIP_NONE 0
#define OPL_CHIP_OPL2 1
#define OPL_CHIP_OPL3 2

typedef struct {
    uint32 chip_type;
    uint32 num_voices;
} opl_info_t;

typedef struct {
    uint32 opl3_mode;
} opl_init_t;

typedef struct {
    uint16 reg;
    uint8 value;
    uint8 reserved;
} opl_write_t;

#define OPL_IOCTL_GET_INFO 1
#define OPL_IOCTL_INIT     2
#define OPL_IOCTL_RESET    3

#endif
