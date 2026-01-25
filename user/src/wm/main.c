// wm.c - improved logging and hardening
#include <system.h>
#include <string.h>
#include <mem.h>
#include <io.h>
#include "fb.h"
#include "protocol.h"
#include "../../libkeyboard/include/keyboard.h"

#ifndef DEBUG
static bool debug = false;
#else
static bool debug = true;
#endif

#define FB_BACKBUFFER_SIZE  (1280 * 800 * sizeof(uint32))
#define ASSERT(expr, msg, ...) do { if (expr) { dprintf("\033[31m[wm]: ERROR: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); return; } } while (0)
#define ERROR(msg, ...) do { dprintf("\033[31m[wm]: ERROR: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); } while (0)
#define WARN(msg, ...) do { if (debug == true) { dprintf("\033[33m[wm]: WARN: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); } } while (0)
#define INFO(msg, ...) do { if (debug == true) { dprintf("[wm]: INFO: "); dprintf(msg, ##__VA_ARGS__); } } while (0)
#define LOG_ERR_RET(expr, msg, ...) do { if (expr) { dprintf("\033[31m[wm]: ERROR: "); dprintf(msg, ##__VA_ARGS__); dprintf("\033[0m"); return -1; } } while (0)

static inline int max(int a, int b) {
    return (a > b) ? a : b;
}

static inline int min(int a, int b) {
    return (a < b) ? a : b;
}

static void client_remove_at(int idx);
static void recompute_layout(uint16 screen_w, uint16 screen_h);
static void client_teardown_by_index(int idx);

void fb_setup(handle_t *h, uint32 **backbuffer) {
    *h = get_obj(INVALID_HANDLE, "$devices/fb0", RIGHT_READ | RIGHT_WRITE);
    ASSERT(*h == INVALID_HANDLE, "Failed to get framebuffer handle\n");
    *backbuffer = malloc(FB_BACKBUFFER_SIZE);
    ASSERT(!*backbuffer, "Failed to allocate global surface\n");
    INFO("Framebuffer and backbuffer setup OK (handle=%d)\n", (int)*h);
}

void server_setup(handle_t *server, handle_t *client) {
    channel_create(server, client);
    ns_register("$gui/wm", *client);
    INFO("Server channel published at $gui/wm\n");
}

wm_client_t clients[16];
uint8 num_clients = 0;
int8 focused = -1;

static void client_remove_at(int idx) {
    if (idx < 0 || idx >= num_clients) return;

    wm_client_t *c = &clients[idx];
    INFO("Tearing down client idx=%d pid=%u\n", idx, c->pid);

    if (c->surface) {
        vmo_unmap(c->surface, (size)c->surface_w * (size)c->surface_h * sizeof(uint32));
        c->surface = NULL;
    }

    if (c->vmo != INVALID_HANDLE) {
        handle_close(c->vmo);
        c->vmo = INVALID_HANDLE;
    }

    if (c->handle != INVALID_HANDLE) {
        handle_close(c->handle);
        c->handle = INVALID_HANDLE;
    }

    for (int j = idx; j + 1 < num_clients; j++) {
        clients[j] = clients[j + 1];
    }
    num_clients--;

    if (num_clients == 0) {
        focused = -1;
    } else if (focused >= num_clients) {
        focused = num_clients - 1;
    }

    recompute_layout(1280, 800);
    INFO("Client removed. remaining=%d focused=%d\n", num_clients, focused);
}

static void client_teardown_by_index(int idx) {
    WARN("client_teardown_by_index: tearing down client at index %d\n", idx);
    client_remove_at(idx);
}

static void recompute_layout(uint16 screen_w, uint16 screen_h) {
    if (num_clients == 0) return;
    INFO("Recomputing layout for %u clients (screen %u x %u)\n", num_clients, screen_w, screen_h);

    uint16 tile_h = screen_h / num_clients;
    for (int i = 0; i < num_clients; i++) {
        wm_client_t *c = &clients[i];
        c->x = 0;
        c->y = i * tile_h;
        c->win_w = screen_w;
        c->win_h = (i == num_clients - 1) ? (screen_h - tile_h * (num_clients - 1)) : tile_h;
        c->dirty = true;

        wm_server_msg_t configure = (wm_server_msg_t){
            .type = CONFIGURE,
            .u.configure = {
                .x = (uint16)c->x,
                .y = (uint16)c->y,
                .w = (uint16)c->win_w,
                .h = (uint16)c->win_h
            }
        };

        int rc = channel_send(c->handle, &configure, sizeof(configure));
        if (rc != 0) {
            WARN("Failed to send CONFIGURE to pid=%u (idx=%d) rc=%d, scheduling teardown\n", c->pid, i, rc);
            client_teardown_by_index(i);
            i--;
            continue;
        } else {
            INFO("Sent CONFIGURE to pid=%u idx=%d => x=%d y=%d w=%d h=%d\n",
                 c->pid, i, c->x, c->y, c->win_w, c->win_h);
            c->status = CONFIGURED;
        }
    }
}

void window_create(handle_t *server, channel_recv_result_t res, wm_client_msg_t req) {
    INFO("window_create from pid=%u requested %u x %u\n", res.sender_pid, req.u.create.width, req.u.create.height);
    if (num_clients == 16) {
        WARN("Max clients reached, ignoring create from pid=%u\n", res.sender_pid);
        return;
    }

    char path[64];
    size needed = (size)req.u.create.width * (size)req.u.create.height * sizeof(uint32);
    handle_t client_vmo = vmo_create(needed, VMO_FLAG_RESIZABLE, RIGHT_MAP);
    if (client_vmo == INVALID_HANDLE) {
        ERROR("vmo_create failed for pid=%u size=%zu\n\033[37m", res.sender_pid, needed);
        return;
    }
    snprintf(path, sizeof(path), "$gui/%u/surface", res.sender_pid);
    ns_register(path, client_vmo);
    INFO("Registered surface %s for pid=%u\n", path, res.sender_pid);

    uint32 *surface = vmo_map(client_vmo, NULL, 0, needed, RIGHT_MAP);
    if (!surface) {
        ERROR("vmo_map failed for pid=%u size=%zu\n\033[37m", res.sender_pid, needed);
        handle_close(client_vmo);
        return;
    }

    handle_t wm_end = INVALID_HANDLE, client_end = INVALID_HANDLE;
    channel_create(&wm_end, &client_end);
    if (wm_end == INVALID_HANDLE || client_end == INVALID_HANDLE) {
        ERROR("channel_create failed for pid=%u\n\033[37m", res.sender_pid);
        vmo_unmap(surface, needed);
        handle_close(client_vmo);
        return;
    }
    snprintf(path, sizeof(path), "$gui/%u/channel", res.sender_pid);
    ns_register(path, client_end);
    INFO("Published channel %s for pid=%u\n", path, res.sender_pid);

    wm_client_t newc = {
        .pid = res.sender_pid,
        .handle = wm_end,
        .surface = surface,
        .vmo = client_vmo,
        .surface_w = req.u.create.width,
        .surface_h = req.u.create.height,
        .win_w = req.u.create.width,
        .win_h = req.u.create.height,
        .x = 0,
        .y = 0,
        .dirty = false,
        .status = CREATED,
    };
    clients[num_clients++] = newc;

    wm_server_msg_t resp = (wm_server_msg_t){ .type = ACK, .u.ack = true };
    channel_send(*server, &resp, sizeof(resp));
    INFO("CREATE handled for pid=%u idx=%d\n", res.sender_pid, num_clients - 1);

    if (focused == -1) focused = 0;
    recompute_layout(1280, 800);
}

void window_commit(handle_t client_handle, channel_recv_result_t res) {
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].pid == res.sender_pid) {
            INFO("Received COMMIT from pid=%u idx=%d\n", res.sender_pid, i);
            if (clients[i].status != READY) {
                WARN("Ignoring COMMIT from pid=%u because status=%d\n", res.sender_pid, clients[i].status);
                return;
            }

            clients[i].dirty = true;
            wm_server_msg_t resp = (wm_server_msg_t){ .type = ACK, .u.ack = true };
            int rc = channel_send(client_handle, &resp, sizeof(resp));
            if (rc != 0) {
                WARN("Failed to ACK COMMIT to pid=%u (rc=%d) - tearing down\n", res.sender_pid, rc);
                for (int j = 0; j < num_clients; j++) {
                    if (clients[j].pid == res.sender_pid) {
                        client_teardown_by_index(j);
                        return;
                    }
                }
            }
            return;
        }
    }
    WARN("COMMIT from unknown pid=%u\n", res.sender_pid);
}

void server_listen(handle_t *server) {
    wm_client_msg_t msg;
    channel_recv_result_t res;

    int s_rc = channel_try_recv_msg(*server, &msg, sizeof(wm_client_msg_t), NULL, 0, &res);
    if (s_rc == 0) {
        switch (msg.type) {
            case CREATE:
                INFO("Received CREATE on $gui/wm from pid=%u\n", res.sender_pid);
                window_create(server, res, msg);
                break;
            default:
                WARN("Unknown message type %d on $gui/wm\n", msg.type);
                break;
        }
    } else if (s_rc < 0 && s_rc != -3) {
        WARN("Error receiving on $gui/wm rc=%d\n", s_rc);
    }

    for (int i = 0; i < num_clients; i++) {
        wm_client_msg_t cmsg;
        channel_recv_result_t cres;
        int rc = channel_try_recv_msg(clients[i].handle, &cmsg, sizeof(wm_client_msg_t), NULL, 0, &cres);
        if (rc != 0) {
            if (rc < 0 && rc != -3) {
                WARN("Channel error or peer closed for pid=%u idx=%d rc=%d - tearing down\n", clients[i].pid, i, rc);
                client_remove_at(i);
                i--;
            }
            continue;
        }

        switch (cmsg.type) {
            case ACK:
                INFO("ACK recv from pid=%u idx=%d status=%d\n", clients[i].pid, i, clients[i].status);
                break;

            case COMMIT:
                window_commit(clients[i].handle, cres);
                break;

            case RESIZE:
                INFO("RESIZE recv from pid=%u requested %u x %u\n", clients[i].pid, cmsg.u.resize.width, cmsg.u.resize.height);
                if (clients[i].status != CONFIGURED) {
                    WARN("Ignoring RESIZE from pid=%u because status=%d\n", clients[i].pid, clients[i].status);
                    break;
                }

                if (clients[i].surface) {
                    vmo_unmap(clients[i].surface, (size)clients[i].surface_w * (size)clients[i].surface_h * sizeof(uint32));
                    clients[i].surface = NULL;
                }
                clients[i].surface_w = cmsg.u.resize.width;
                clients[i].surface_h = cmsg.u.resize.height;
                size needed = (size)clients[i].surface_w * (size)clients[i].surface_h * sizeof(uint32);
                clients[i].surface = vmo_map(clients[i].vmo, NULL, 0, needed, RIGHT_MAP);
                if (!clients[i].surface) {
                    ERROR("vmo_map failed for pid=%u idx=%d\n\033[37m", clients[i].pid, i);
                    client_remove_at(i);
                    i--;
                    break;
                }
                clients[i].dirty = true;
                INFO("Resized and remapped pid=%u idx=%d -> %u x %u\n", clients[i].pid, i, clients[i].surface_w, clients[i].surface_h);
                clients[i].status = READY;
                break;

            case KBD:
                WARN("Unexpected KBD message from client pid=%u\n", clients[i].pid);
                break;

            default:
                WARN("Unknown client msg type=%d from pid=%u\n", cmsg.type, clients[i].pid);
                break;
        }
    }
}

void render_surfaces(handle_t fb_handle, uint32 *fb_backbuffer) {
    if (num_clients < 1) return;
    memset(fb_backbuffer, 0x00, FB_BACKBUFFER_SIZE);

    for (int i = 0; i < num_clients; i++) {
        wm_client_t *c = &clients[i];
        if (!c->dirty) continue;
        if (c->status != READY) continue;
        if (!c->surface) {
            WARN("Skipping client idx=%d pid=%u because surface == NULL\n", i, c->pid);
            continue;
        }

        int dst_x0 = max((int)c->x, 0);
        int dst_y0 = max((int)c->y, 0);
        int dst_x1 = min((int)(c->x + c->win_w), FB_W);
        int dst_y1 = min((int)(c->y + c->win_h), FB_H);
        if (dst_x0 >= dst_x1 || dst_y0 >= dst_y1) continue;

        int copy_w = dst_x1 - dst_x0;
        int copy_h = dst_y1 - dst_y0;

        int src_x0 = dst_x0 - (int)c->x;
        int src_y0 = dst_y0 - (int)c->y;

        if (src_x0 < 0) src_x0 = 0;
        if (src_y0 < 0) src_y0 = 0;
        if (copy_w > (int)(c->surface_w - src_x0)) copy_w = (int)(c->surface_w - src_x0);
        if (copy_h > (int)(c->surface_h - src_y0)) copy_h = (int)(c->surface_h - src_y0);

        if (copy_w <= 0 || copy_h <= 0) continue;

        INFO("Compositing pid=%u idx=%d dst=(%d,%d) size=(%d,%d) src=(%d,%d)\n",
             c->pid, i, dst_x0, dst_y0, copy_w, copy_h, src_x0, src_y0);

        for (int row = 0; row < copy_h; row++) {
            uint32 *src_row = c->surface + (size)(src_y0 + row) * c->surface_w + src_x0;
            uint32 *dst_row = fb_backbuffer + (size)(dst_y0 + row) * FB_W + dst_x0;
            memcpy(dst_row, src_row, (size)copy_w * sizeof(uint32));
        }

        c->dirty = false;
    }

    handle_seek(fb_handle, 0, HANDLE_SEEK_SET);
    handle_write(fb_handle, fb_backbuffer, FB_BACKBUFFER_SIZE);
}

void handle_input() {
    if (focused == -1) return;
    kbd_event_t ev;
    if (kbd_try_read(&ev) == 0) {
        if (focused < 0 || focused >= num_clients) {
            WARN("Got keyboard event but focused index invalid: %d\n", focused);
            return;
        }
        wm_server_msg_t msg = {
            .type = KBD,
            .u.kbd = { .data = ev },
        };
        int rc = channel_send(clients[focused].handle, &msg, sizeof(msg));
        if (rc != 0) {
            WARN("Failed to forward keyboard to pid=%u idx=%d rc=%d - tearing down\n", clients[focused].pid, focused, rc);
            client_teardown_by_index(focused);
        } else {
            INFO("Forwarded keyboard to pid=%u idx=%d key=%u down=%u\n", clients[focused].pid, focused, ev.codepoint, ev.pressed);
        }
    }
}

int main(void) {
    handle_t fb_handle = INVALID_HANDLE;
    uint32 *fb_backbuffer = NULL;
    handle_t server_handle = INVALID_HANDLE;
    handle_t client_handle = INVALID_HANDLE;

    fb_setup(&fb_handle, &fb_backbuffer);
    INFO("Setup framebuffer handle %d\n", (int)fb_handle);
    kbd_init();
    INFO("Setup keyboard\n");
    server_setup(&server_handle, &client_handle);
    INFO("Setup server channel\n");

    while (1) {
        server_listen(&server_handle);
        handle_input();
        render_surfaces(fb_handle, fb_backbuffer);

        yield();
    }
    __builtin_unreachable();
    return 0;
}
