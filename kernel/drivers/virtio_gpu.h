#ifndef DRIVERS_VIRTIO_GPU_H
#define DRIVERS_VIRTIO_GPU_H

#include <arch/types.h>
#include "virtio/virtio.h"

//virtio-gpu control command types
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO     0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF       0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT          0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH       0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107

//virtio-gpu response types
#define VIRTIO_GPU_RESP_OK_NODATA           0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO     0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC          0x1200

//pixel formats
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM    1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM    67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM    68

#define VIRTIO_GPU_MAX_SCANOUTS 16

//common command header
typedef struct {
    uint32 type;
    uint32 flags;
    uint64 fence_id;
    uint32 ctx_id;
    uint32 _padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

typedef struct {
    uint32 x, y, width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

//GET_DISPLAY_INFO response
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    struct {
        virtio_gpu_rect_t r;
        uint32 enabled;
        uint32 flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

//RESOURCE_CREATE_2D command
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32 resource_id;
    uint32 format;
    uint32 width;
    uint32 height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

//RESOURCE_ATTACH_BACKING command (followed by nr_entries virtio_gpu_mem_entry_t structs)
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32 resource_id;
    uint32 nr_entries;
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

//scatter-gather entry for backing memory
typedef struct {
    uint64 addr;        //physical address of backing page(s)
    uint32 length;      //length of this segment
    uint32 _padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

//SET_SCANOUT command
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint32 scanout_id;
    uint32 resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

//RESOURCE_FLUSH command
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint32 resource_id;
    uint32 _padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

//TRANSFER_TO_HOST_2D command
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint64 offset;
    uint32 resource_id;
    uint32 _padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

//driver init (BUS level so it runs before fb_init_backbuffer)
void virtio_gpu_init(void);

#endif
