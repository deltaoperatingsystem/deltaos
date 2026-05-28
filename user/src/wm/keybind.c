#include "keybind.h"
#include "layout.h"

void handle_key_event(comp_msg_t *msg) {
    kbd_event_t *ev = &msg->u.key_event.data;

    //the WM only consumes a small shortcut set and forwards the rest unchanged
    //bound keys are press only actions
    if (ev->pressed) {
        //alt+m quits the WM and leaves the compositor in fallback mode
        if (ev->mods == KBD_MOD_ALT && ev->codepoint == 'm') {
            INFO("Alt+M - exiting\n");
            comp_unclaim_wm(wm_ch);
            exit(0);
        }

        if (ev->mods == KBD_MOD_ALT && ev->codepoint == '\t') {
            if (num_clients == 0) return;
            surface_id_t next_id = clients[0].id;
            raise_client(next_id);
            focused = next_id;
            comp_set_focus(wm_ch, focused);
            return;
        }

        //alt+shift+Q kills the focused client
        if (ev->mods == (KBD_MOD_ALT | KBD_MOD_SHIFT) && ev->codepoint == 'Q') {
            int idx = find_client(focused);
            if (idx >= 0) {
                uint32 pid = clients[idx].pid;
                surface_id_t sid = clients[idx].id;
                comp_wm_destroy_surface(wm_ch, sid);
                proc_send_event(pid, PROC_EVENT_TERMINATE);
                remove_client(idx);
            }
            return;
        }

        //alt+A spawns the demo app
        if (ev->mods == KBD_MOD_ALT && ev->codepoint == 'a') {
            spawn("/system/binaries/app", 0, NULL);
            return;
        }
    }

    //unhandled keys press or release are forwarded to the focused surface
    if (focused != 0) {
        comp_pass_through(wm_ch, focused, *ev);
    }
}
