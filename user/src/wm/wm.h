#ifndef WM_H
#define WM_H

#include <system.h>
#include <io.h>
#include <math.h>
#include <compositor/protocol.h>
#include <compositor/client.h>

#define ASSERT(expr, ...) \
    do { \
        if (expr) { \
            dprintf("\033[31m[wm]: ERROR: "); \
            dprintf(__VA_ARGS__); \
            dprintf("\033[0m"); \
            exit(1); \
        } \
    } while (0)

#define WARN(...) \
    do { \
        dprintf("\033[33m[wm]: WARN: "); \
        dprintf(__VA_ARGS__); \
        dprintf("\033[0m"); \
    } while (0)

#define INFO(...) \
    do { \
        dprintf("[wm]: INFO: "); \
        dprintf(__VA_ARGS__); \
    } while (0)

#define FB_RGB(r, g, b) ((uint32)((b) | ((g) << 8) | ((r) << 16)))

#define MAX_CLIENTS 16
#define SCREEN_W 1280
#define SCREEN_H 800
#define TITLEBAR_H 22
#define BORDER_W 2

typedef struct {
    surface_id_t id;
    uint32 pid;
    uint16 w, h;
    int16 x, y;
    //content size excludes the titlebar and outer borders
    uint16 content_w, content_h;
    char title[32];
    bool alive;
} wm_client_t;

//wm state is global because the process is single threaded
extern handle_t wm_ch;
extern wm_client_t clients[MAX_CLIENTS];
extern uint8 num_clients;
extern surface_id_t focused;

//helpers shared across the layout and keybind modules
int find_client(surface_id_t id);
void send_decoration(surface_id_t id);
void remove_client(int idx);
void recompute_layout(void);
void raise_client(surface_id_t id);
void handle_key_event(comp_msg_t *msg);

#endif
