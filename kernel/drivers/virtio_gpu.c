#include <drivers/virtio_gpu.h>
#include <drivers/virtio/virtio.h>
#include <drivers/fb.h>
#include <drivers/init.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/mm.h>
#include <mm/kheap.h>
#include <arch/mmu.h>
#include <lib/io.h>
#include <lib/string.h>
#include <arch/cpu.h>
#include <proc/thread.h>
#include <proc/wait.h>
#include <lib/spinlock.h>

//sleeping mutex: spinlock guards the state, wait_queue parks contending threads
typedef struct {
    spinlock_t lock;
    wait_queue_t wq;
    bool held;
} gpu_mutex_t;

static void gpu_mutex_init(gpu_mutex_t *m) {
    spinlock_init(&m->lock);
    wait_queue_init(&m->wq);
    m->held = false;
}

static void gpu_mutex_acquire(gpu_mutex_t *m) {
    spinlock_acquire(&m->lock);
    while (m->held) {
        thread_sleep_locked(&m->wq, &m->lock);
        spinlock_acquire(&m->lock);
    }
    m->held = true;
    spinlock_release(&m->lock);
}

static void gpu_mutex_release(gpu_mutex_t *m) {
    spinlock_acquire(&m->lock);
    m->held = false;
    thread_wake_one(&m->wq);
    spinlock_release(&m->lock);
}

//virtual address region reserved for GPU framebuffer mappings
//sits above the kernel heap, well away from the HHDM and kheap ranges
#define GPU_VMAP_BASE  0xFFFFA00000000000ULL
#define GPU_VMAP_LIMIT 0xFFFFB00000000000ULL

static uintptr gpu_vmap_cursor = GPU_VMAP_BASE;

//carve out `pages` pages of contiguous VA space and map each physical page into it
static void *gpu_vmap(void **phys_pages, uint32 pages) {
    uintptr virt = gpu_vmap_cursor;
    if (virt + (uintptr)pages * 0x1000 > GPU_VMAP_LIMIT) return NULL;
    gpu_vmap_cursor += (uintptr)pages * 0x1000;

    for (uint32 i = 0; i < pages; i++) {
        vmm_kernel_map(virt + (uintptr)i * 0x1000,
                       (uintptr)phys_pages[i],
                       1, MMU_FLAG_WRITE);
    }
    return (void *)virt;
}

#define GPU_CONTROLQ  0 //virtqueue 0 is the control queue
#define GPU_CURSORQ 1 //virtqueue 1 is the cursor queue (unused here)
#define GPU_QUEUE_SIZE 64
#define GPU_RESOURCE_ID 1 //we use a single resource for scanout 0

//per-command DMA buffer: holds the request and response back-to-back
//keeping them in a single physically contiguous allocation simplifies
//the descriptor chain (req -> resp in two descriptors)
#define CMD_BUF_PAGES   18
#define CMD_BUF_SIZE    (CMD_BUF_PAGES * 4096)

//max number of scatter-gather backing entries we'll use
//one entry per 4096-byte page, sized for up to 4K at 32 bpp
#define GPU_MAX_BACKING_ENTRIES 4096

typedef struct {
    virtio_device_t *vdev;
    virtq_t ctrlq;

    //command buffer (physically contiguous, used for all control commands)
    void *cmd_virt;
    uintptr cmd_phys;

    //framebuffer backing memory (scatter-gather across multiple pages)
    uint32 width;
    uint32 height;
    uint32 pitch;
    size fb_size;

    void **backing_virt; //virtual addresses of backing pages
    void **backing_phys; //physical addresses of backing pages
    uint32 backing_pages; //number of backing pages

    void *fb_linear;      //linear virtual view of the framebuffer (vmalloc'd contiguous mapping of the pages)

    fb_backend_t backend;
} gpu_dev_t;

static gpu_dev_t *gpu = NULL;
static gpu_mutex_t gpu_mutex;

//async flush state: a dedicated flush request lives permanently in the cmd buffer
//offset from cmd_virt so it doesn't clash with the general command area
#define FLUSH_CMD_OFFSET  (CMD_BUF_SIZE - 2048)

//send a control command and wait for its response
//req_virt/resp_virt are virtual addresses in the cmd buffer
//req_phys/resp_phys are their physical counterparts
static int gpu_submit_cmd(gpu_dev_t *g,
    void *req_virt, uintptr req_phys, uint32 req_len,
    void *resp_virt, uintptr resp_phys, uint32 resp_len) {
    (void)req_virt;

    //serialize: one command in flight at a time
    //this covers both the shared cmd buffer and last_used_idx in virtq_poll_used
    gpu_mutex_acquire(&gpu_mutex);

    spinlock_acquire(&g->ctrlq.lock);

    int req_desc = virtq_alloc_desc(&g->ctrlq);
    int rsp_desc = virtq_alloc_desc(&g->ctrlq);
    if (req_desc < 0 || rsp_desc < 0) {
        if (req_desc >= 0) virtq_free_desc(&g->ctrlq, (uint16)req_desc);
        if (rsp_desc >= 0) virtq_free_desc(&g->ctrlq, (uint16)rsp_desc);
        spinlock_release(&g->ctrlq.lock);
        gpu_mutex_release(&gpu_mutex);
        return -1;
    }

    //request descriptor (device reads it)
    g->ctrlq.desc[req_desc].addr = req_phys;
    g->ctrlq.desc[req_desc].len = req_len;
    g->ctrlq.desc[req_desc].flags = VIRTQ_DESC_F_NEXT;
    g->ctrlq.desc[req_desc].next = (uint16)rsp_desc;

    //response descriptor (device writes into it)
    g->ctrlq.desc[rsp_desc].addr = resp_phys;
    g->ctrlq.desc[rsp_desc].len = resp_len;
    g->ctrlq.desc[rsp_desc].flags = VIRTQ_DESC_F_WRITE;
    g->ctrlq.desc[rsp_desc].next = 0;

    uint16 head = (uint16)req_desc;
    virtq_kick(g->vdev, &g->ctrlq, head);

    spinlock_release(&g->ctrlq.lock);
    //poll until the device posts a used-ring entry for our chain
    int ret = virtq_poll_used(&g->ctrlq, head);

    spinlock_acquire(&g->ctrlq.lock);
    virtq_free_desc(&g->ctrlq, (uint16)req_desc);
    virtq_free_desc(&g->ctrlq, (uint16)rsp_desc);
    spinlock_release(&g->ctrlq.lock);

    gpu_mutex_release(&gpu_mutex);

    if (ret < 0) {
        printf("[virtio-gpu] submit_cmd: poll_used timed out\n");
        return -1;
    }
    //check the response type   
    virtio_gpu_ctrl_hdr_t *resp = resp_virt;
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA &&
        resp->type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        printf("[virtio-gpu] submit_cmd: unexpected response type 0x%04X\n", resp->type);
        return -1;
    }
    return 0;
}

//get display info and fill in g->width/height from scanout 0
static int gpu_get_display_info(gpu_dev_t *g) {
    virtio_gpu_ctrl_hdr_t *req  = g->cmd_virt;
    virtio_gpu_resp_display_info_t *resp =
        (virtio_gpu_resp_display_info_t *)((uint8 *)g->cmd_virt + sizeof(virtio_gpu_ctrl_hdr_t));

    memset(req, 0, sizeof(*req));
    req->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uintptr req_phys  = g->cmd_phys;
    uintptr resp_phys = req_phys + sizeof(virtio_gpu_ctrl_hdr_t);

    if (gpu_submit_cmd(g,
                       req,  req_phys,  sizeof(*req),
                       resp, resp_phys, sizeof(*resp)) != 0) {
        return -1;
    }

    if (!resp->pmodes[0].enabled) {
        printf("[virtio-gpu] scanout 0 not enabled, using fallback 1024x768\n");
        g->width  = 1024;
        g->height = 768;
    } else {
        g->width  = resp->pmodes[0].r.width;
        g->height = resp->pmodes[0].r.height;
    }

    g->pitch   = g->width * 4; //B8G8R8A8 = 4 bytes per pixel
    g->fb_size = g->height * g->pitch;
    return 0;
}

//create the 2D resource on the host
static int gpu_resource_create(gpu_dev_t *g) {
    virtio_gpu_resource_create_2d_t *req = g->cmd_virt;
    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)((uint8 *)g->cmd_virt + sizeof(*req));

    memset(req, 0, sizeof(*req));
    req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id = GPU_RESOURCE_ID;
    req->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    req->width = g->width;
    req->height = g->height;

    uintptr req_phys = g->cmd_phys;
    uintptr resp_phys = req_phys + sizeof(*req);

    return gpu_submit_cmd(g,
                          req,  req_phys,  sizeof(*req),
                          resp, resp_phys, sizeof(*resp));
}

//allocate scatter-gather backing pages and attach them to the resource
static int gpu_attach_backing(gpu_dev_t *g) {
    uint32 pages = (uint32)((g->fb_size + 0xFFF) / 0x1000);
    if (pages > GPU_MAX_BACKING_ENTRIES) return -1;

    g->backing_pages = pages;
    g->backing_virt = kzalloc(pages * sizeof(void *));
    g->backing_phys = kzalloc(pages * sizeof(void *));
    if (!g->backing_virt || !g->backing_phys) return -1;

    for (uint32 i = 0; i < pages; i++) {
        g->backing_phys[i] = pmm_alloc(1);
        if (!g->backing_phys[i]) return -1;
        g->backing_virt[i] = P2V(g->backing_phys[i]);
        memset(g->backing_virt[i], 0, 0x1000);
    }

    //build the ATTACH_BACKING command + mem_entry array in the cmd buffer
    //layout: [attach_hdr][mem_entry_0]...[mem_entry_N-1][resp]
    virtio_gpu_resource_attach_backing_t *cmd = g->cmd_virt;
    virtio_gpu_mem_entry_t *entries = (virtio_gpu_mem_entry_t *)(cmd + 1);
    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(entries + pages);

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = GPU_RESOURCE_ID;
    cmd->nr_entries = pages;

    for (uint32 i = 0; i < pages; i++) {
        entries[i].addr = (uint64)(uintptr)g->backing_phys[i];
        entries[i].length = 0x1000;
        entries[i]._padding = 0;
    }

    uint32 cmd_len = sizeof(*cmd) + pages * sizeof(virtio_gpu_mem_entry_t);
    uintptr cmd_phys = g->cmd_phys;
    uintptr resp_phys = cmd_phys + cmd_len;

    int ret = gpu_submit_cmd(g,
                             cmd,  cmd_phys,  cmd_len,
                             resp, resp_phys, sizeof(*resp));
    if (ret != 0) return ret;

    //map the scatter-gather backing pages into a contiguous virtual region
    //so all drawing code sees one flat framebuffer regardless of whether
    //the physical pages are contiguous
    g->fb_linear = gpu_vmap(g->backing_phys, pages);
    if (!g->fb_linear) {
        printf("[virtio-gpu] ERR: out of GPU vmap space\n");
        return -1;
    }

    return 0;
}

//set scanout 0 to display our resource
static int gpu_set_scanout(gpu_dev_t *g) {
    virtio_gpu_set_scanout_t *req  = g->cmd_virt;
    virtio_gpu_ctrl_hdr_t    *resp = (virtio_gpu_ctrl_hdr_t *)((uint8 *)g->cmd_virt + sizeof(*req));

    memset(req, 0, sizeof(*req));
    req->hdr.type   = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.x        = 0;
    req->r.y        = 0;
    req->r.width    = g->width;
    req->r.height   = g->height;
    req->scanout_id  = 0;
    req->resource_id = GPU_RESOURCE_ID;

    uintptr req_phys  = g->cmd_phys;
    uintptr resp_phys = req_phys + sizeof(*req);

    return gpu_submit_cmd(g,
                          req,  req_phys,  sizeof(*req),
                          resp, resp_phys, sizeof(*resp));
}

//transfer backing memory to the host and flush to scanout
//rect specifies which portion of the screen to update
static int gpu_flush_rect_internal(gpu_dev_t *g,
                                    uint32 x, uint32 y,
                                    uint32 w, uint32 h) {
    //we need two commands: TRANSFER_TO_HOST_2D and RESOURCE_FLUSH
    //pack them back-to-back in the cmd buffer at FLUSH_CMD_OFFSET
    virtio_gpu_transfer_to_host_2d_t *xfer =
        (virtio_gpu_transfer_to_host_2d_t *)((uint8 *)g->cmd_virt + FLUSH_CMD_OFFSET);
    virtio_gpu_ctrl_hdr_t *xfer_resp = (virtio_gpu_ctrl_hdr_t *)(xfer + 1);

    memset(xfer, 0, sizeof(*xfer));
    xfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer->r.x = x;
    xfer->r.y = y;
    xfer->r.width = w;
    xfer->r.height = h;
    xfer->offset = y * g->pitch + x * 4;
    xfer->resource_id = GPU_RESOURCE_ID;

    uintptr xfer_phys = g->cmd_phys + FLUSH_CMD_OFFSET;
    uintptr xfer_resp_phys = xfer_phys + sizeof(*xfer);

    if (gpu_submit_cmd(g,
                       xfer,      xfer_phys,      sizeof(*xfer),
                       xfer_resp, xfer_resp_phys, sizeof(*xfer_resp)) != 0) {
        return -1;
    }

    //now RESOURCE_FLUSH
    virtio_gpu_resource_flush_t *flush = (virtio_gpu_resource_flush_t *)(xfer_resp + 1);
    virtio_gpu_ctrl_hdr_t *flush_resp = (virtio_gpu_ctrl_hdr_t *)(flush + 1);

    memset(flush, 0, sizeof(*flush));
    flush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r.x = x;
    flush->r.y = y;
    flush->r.width = w;
    flush->r.height = h;
    flush->resource_id = GPU_RESOURCE_ID;

    uintptr flush_phys = xfer_resp_phys + sizeof(*xfer_resp);
    uintptr flush_resp_phys = flush_phys     + sizeof(*flush);

    return gpu_submit_cmd(g, flush,      flush_phys,      sizeof(*flush),
                          flush_resp, flush_resp_phys, sizeof(*flush_resp));
}

//fb_backend flush callbacks
static void gpu_flush(void) {
    if (!gpu) return;
    gpu_flush_rect_internal(gpu, 0, 0, gpu->width, gpu->height);
}

static void gpu_flush_rect(uint32 x, uint32 y, uint32 w, uint32 h) {
    if (!gpu) return;
    gpu_flush_rect_internal(gpu, x, y, w, h);
}

//BUS-level init so we beat fb_init_backbuffer's DEVICE-level fallback
void virtio_gpu_init(void) {
    //find the first virtio GPU device in the global list
    virtio_device_t *vdev = NULL;
    for (virtio_device_t *d = virtio_devices; d; d = d->next) {
        if (d->device_type == VIRTIO_DEV_GPU) {
            vdev = d;
            break;
        }
    }

    if (!vdev) {
        printf("[virtio-gpu] no device found\n");
        return;
    }

    gpu_dev_t *g = kzalloc(sizeof(gpu_dev_t));
    if (!g) return;

    gpu_mutex_init(&gpu_mutex);

    g->vdev = vdev;

    //allocate command buffer (one physically contiguous page)
    g->cmd_phys = (uintptr)pmm_alloc(CMD_BUF_PAGES);
    if (!g->cmd_phys) { kfree(g); return; }
    g->cmd_virt = P2V((void *)g->cmd_phys);
    memset(g->cmd_virt, 0, CMD_BUF_SIZE);

    //standard virtio init sequence, we don't need any optional features for 2D
    if (virtio_dev_init(vdev, VIRTIO_F_VERSION_1) != 0) {
        printf("[virtio-gpu] ERR: device init failed\n");
        goto fail;
    }

    //set up the control queue
    if (vdev->transport->setup_queue(vdev, GPU_CONTROLQ, GPU_QUEUE_SIZE, &g->ctrlq) != 0) {
        printf("[virtio-gpu] ERR: failed to set up control queue\n");
        goto fail;
    }

    //driver OK
    vdev->transport->write_status(vdev,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    //query display dimensions
    if (gpu_get_display_info(g) != 0) {
        printf("[virtio-gpu] ERR: GET_DISPLAY_INFO failed\n");
        goto fail;
    }

    printf("[virtio-gpu] display: %ux%u\n", g->width, g->height);

    //create the 2D resource
    if (gpu_resource_create(g) != 0) {
        printf("[virtio-gpu] ERR: RESOURCE_CREATE_2D failed\n");
        goto fail;
    }

    //allocate backing pages and attach them
    if (gpu_attach_backing(g) != 0) {
        printf("[virtio-gpu] ERR: RESOURCE_ATTACH_BACKING failed\n");
        goto fail;
    }

    //bind resource to scanout 0
    if (gpu_set_scanout(g) != 0) {
        printf("[virtio-gpu] ERR: SET_SCANOUT failed\n");
        goto fail;
    }

    //register the fb backend so fb_init_backbuffer() uses us instead of GOP
    g->backend.name = "virtio-gpu";
    g->backend.draw_buffer = g->fb_linear;
    g->backend.display_buffer = g->fb_linear;  //host sees it via backing pages
    g->backend.width = g->width;
    g->backend.height = g->height;
    g->backend.pitch = g->pitch;
    g->backend.size = g->fb_size;
    g->backend.flush = gpu_flush;
    g->backend.flush_rect = gpu_flush_rect;
    g->backend.cleanup = NULL;

    if (!fb_set_backend(&g->backend)) {
        printf("[virtio-gpu] WARN: fb backend already claimed, running headless\n");
    }

    gpu = g;
    printf("[virtio-gpu] initialised: %ux%u backing=%u pages\n",
           g->width, g->height, g->backing_pages);
    return;

fail:
    if (g->cmd_phys) pmm_free((void *)g->cmd_phys, CMD_BUF_PAGES);
    kfree(g);
}

DECLARE_DRIVER(virtio_gpu_init, INIT_LEVEL_BUS);
