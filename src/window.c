/*
 * src/window.c — per-window state management for evilWay.
 *
 * In this phase the translation unit is intentionally thin: only
 * ew_window_state_init lives here. Geometry mutation (toplevel_apply_geometry,
 * do_keyboard_move, etc.) stays in main.c because it needs direct access to
 * Server, wlr_scene, and wlr_seat — pulling those in here would create a
 * circular dependency with evilway.h.
 *
 * Future phases that need per-window helpers independent of compositor state
 * (e.g. vdesk membership queries, fixed-window predicates) should be added
 * here.
 */

#include <string.h>

#include "window.h"

void ew_window_state_init(EwWindowState *ws) {
    memset(ws, 0, sizeof(*ws));
    /* vdesk 0 is the first virtual desktop; windows start there. */
    ws->vdesk = 0;
    /* geom, saved_geom are zero — main.c sets real geometry on map. */
    /* max_flags = 0: not maximized. fixed = false: not fixed. */
}
