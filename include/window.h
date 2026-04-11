/* include/window.h — per-window state for evilWay
 *
 * EwWindowState holds the mutable runtime state for one toplevel window:
 * geometry, pre-maximize save, maximization flags, fixed-window flag, and
 * virtual desktop number. It lives embedded in struct Toplevel (not heap).
 *
 * Keeping this in a separate translation unit from main.c:
 *   (a) makes the state contract explicit and reviewable,
 *   (b) allows future phases (layer-shell, vdesk logic) to include only
 *       what they need without pulling in the full compositor state.
 *
 * DESIGN: geom is the OUTER geometry — it includes the border_width on all
 * four sides. The client content area is:
 *   x + bw, y + bw,  width - 2*bw,  height - 2*bw
 * This matches dwl's convention and simplifies border-rect sizing.
 */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include <wlr/util/box.h>

/*
 * EwWindowState — mutable per-window state.
 *
 * max_flags uses FLAG_VERTICAL and FLAG_HORIZONTAL from evilway.h.
 * Both can be set simultaneously (full-monitor maximize).
 *
 * saved_geom is only meaningful when max_flags != 0. It holds the
 * pre-maximize outer geometry for restore. It is written exactly once
 * per "enter maximize" transition — when going from no-maximize to the
 * first active axis. Subsequent axis toggles share the same save so that
 * restore always returns to the original unmaximized position/size.
 *
 * vdesk is stored here for future virtual-desktop support. It is not
 * acted upon in this phase — all windows share desktop 0 until FUNC_VDESK
 * is implemented.
 */
typedef struct {
    struct wlr_box geom;        /* outer geometry (layout coords, includes border) */
    struct wlr_box saved_geom;  /* pre-maximize save; valid when max_flags != 0 */
    uint32_t       max_flags;   /* FLAG_VERTICAL | FLAG_HORIZONTAL (from evilway.h) */
    bool           fixed;       /* sticky across virtual desktops (fix,toggle) */
    int            vdesk;       /* virtual desktop (0-based; stored, not yet used) */
} EwWindowState;

/*
 * ew_window_state_init — zero-initialize a window state, setting safe defaults.
 *
 * Must be called before a Toplevel is placed on the toplevels list. geom is
 * left as zero-box; the compositor writes real geometry on map.
 */
void ew_window_state_init(EwWindowState *ws);

#endif /* WINDOW_H */
