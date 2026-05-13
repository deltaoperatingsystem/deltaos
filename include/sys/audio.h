#ifndef SYS_AUDIO_H
#define SYS_AUDIO_H

#include <sys/types.h>

#define AUDIO_FORMAT_PCM_8BIT  0
#define AUDIO_FORMAT_PCM_16BIT 1

typedef struct {
    uint16 channels;     //1 = mono, 2 = stereo
    uint16 format;       //AUDIO_FORMAT_PCM_8BIT or AUDIO_FORMAT_PCM_16BIT
    uint32 sample_rate;  //e.x 44100
} audio_format_t;

#define AUDIO_IOCTL_SET_FORMAT 1
#define AUDIO_IOCTL_GET_FORMAT 2
#define AUDIO_IOCTL_START      3
#define AUDIO_IOCTL_STOP       4

#endif
