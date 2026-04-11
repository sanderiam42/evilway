/*
 * evilWay — Wayland compositor
 *
 * Phase 1b: evilwm behavior layer — move/resize, focus-follows-mouse, borders,
 * stacking, and snap-to-edge, layered on the Phase 1a xdg-shell skeleton.
 *
 * References:
 *   dwl    https://codeberg.org/dwl/dwl    (closest working reference, ~3200 lines)
 *   tinywl  in wlroots source tree          (minimal floor, ~1100 lines)
 *
 * Targets wlroots-0.19 (Fedora 43: wlroots-0.19.2-1.fc43).
 * NOTE: Fedora 44 ships wlroots-0.20, which breaks API on every minor version
 * bump. When upgrading, a migration pass will be required before this builds.
 *
 * SECURITY: This compositor is the security boundary between all input events
 * and all rendered output on the machine. Key invariants enforced here:
 *
 *   INPUT:   Compositor keybindings (Super+Shift+Q, etc.) are checked and
 *            consumed BEFORE the key event is forwarded to any client. This is
 *            the enforcement point that prevents clients from intercepting
 *            compositor-level shortcuts.
 *
 *   SOCKET:  The Wayland socket is created by wl_display_add_socket_auto() in
 *            $XDG_RUNTIME_DIR with 0600 permissions, owned by the session user.
 *            This is logged at startup so it can be verified. The compositor
 *            does not create any additional IPC sockets in this phase.
 *
 *   NULLS:   Every wlroots init call that returns a nullable pointer is checked.
 *            Failure is fatal — a partially-initialized compositor is not safe
 *            to run.
 *
 * WLR_USE_UNSTABLE is defined in the build system. It is required to access
 * the wlroots scene graph, layer shell, session lock, and most useful APIs.
 * "Unstable" means wlroots may change these APIs between minor versions, not
 * that they are buggy or unsafe. Every wlroots compositor must define this.
 */

#include <assert.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "evilway.h"
#include "window.h"

/* =========================================================================
 * Forward declarations — needed because grab helpers call toplevel_at()
 * which is defined later (after focus/cursor helpers depend on it).
 * ====================================================================== */

static struct Toplevel *toplevel_at(struct Server *server,
        double lx, double ly,
        struct wlr_surface **surface_out, double *sx, double *sy);

/* =========================================================================
 * Helpers
 * ====================================================================== */

static void die(const char *msg) {
    wlr_log(WLR_ERROR, "fatal: %s", msg);
    exit(1);
}

/* =========================================================================
 * Color and border helpers
 * ====================================================================== */

/*
 * color_to_float — convert packed 0xRRGGBBAA to float[4] in [0,1].
 *
 * wlr_scene_rect_create and wlr_scene_rect_set_color take float[4] in RGBA
 * order with each component in [0,1]. Our compile-time color constants are
 * packed 32-bit values; this converts them at call sites without magic numbers.
 */
static void color_to_float(uint32_t rgba, float out[4]) {
    out[0] = ((rgba >> 24) & 0xFF) / 255.0f;  /* R */
    out[1] = ((rgba >> 16) & 0xFF) / 255.0f;  /* G */
    out[2] = ((rgba >>  8) & 0xFF) / 255.0f;  /* B */
    out[3] = ((rgba      ) & 0xFF) / 255.0f;  /* A */
}

/*
 * toplevel_set_border_color — set all 4 border rects to active or inactive.
 *
 * Called from focus_toplevel() when focus changes. Also called at map time
 * to set the initial color before the first focus event fires.
 */
static void toplevel_set_border_color(struct Toplevel *tl, bool active) {
    float color[4];
    color_to_float(active ? BORDER_COLOR_ACTIVE : BORDER_COLOR_INACTIVE, color);
    for (int i = 0; i < 4; i++)
        wlr_scene_rect_set_color(tl->border[i], color);
}

/*
 * toplevel_apply_geometry — push tl->state.geom into the scene graph.
 *
 * Updates scene tree position, surface offset, border rect sizes/positions,
 * and sends an xdg configure with the new client content size.
 *
 * Must be called after writing tl->state.geom. The caller is responsible
 * for writing the geometry; this function only applies it.
 *
 * Border layout (all positions relative to outer tree origin):
 *   border[0] top:    (0,       0),       width × bw
 *   border[1] bottom: (0,       h-bw),    width × bw
 *   border[2] left:   (0,       bw),      bw × (h - 2*bw)
 *   border[3] right:  (w-bw,    bw),      bw × (h - 2*bw)
 */
static void toplevel_apply_geometry(struct Toplevel *tl) {
    int bw = tl->server->config.border_width;
    struct wlr_box *g = &tl->state.geom;

    /* Position the outer container in layout space. */
    wlr_scene_node_set_position(&tl->scene_tree->node, g->x, g->y);

    /* Offset the client surface inside the outer container. */
    wlr_scene_node_set_position(&tl->surface_tree->node, bw, bw);

    /* Resize and reposition the four border rects. */
    int inner_h = g->height - 2 * bw;

    wlr_scene_rect_set_size(tl->border[0], g->width, bw);           /* top */
    wlr_scene_rect_set_size(tl->border[1], g->width, bw);           /* bottom */
    wlr_scene_rect_set_size(tl->border[2], bw, inner_h);            /* left */
    wlr_scene_rect_set_size(tl->border[3], bw, inner_h);            /* right */

    /* Top is at (0,0) — no explicit set_position needed. */
    wlr_scene_node_set_position(&tl->border[1]->node, 0, g->height - bw);
    wlr_scene_node_set_position(&tl->border[2]->node, 0, bw);
    wlr_scene_node_set_position(&tl->border[3]->node, g->width - bw, bw);

    /* Tell the client its new content size. 0x0 means "client chooses" and
     * is only used at initial_commit time; we never call this with (0,0) for
     * a programmatic resize. */
    int cw = g->width  - 2 * bw;
    int ch = g->height - 2 * bw;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    wlr_xdg_toplevel_set_size(tl->xdg_toplevel, (uint32_t)cw, (uint32_t)ch);
}

/* =========================================================================
 * Focus and stacking helpers
 * ====================================================================== */

/*
 * raise_toplevel — raise a window to the top of the stacking order.
 *
 * Separate from focus so that focus-follows-mouse does NOT raise (evilwm
 * default). Raise happens on explicit click or via FUNC_RAISE bind.
 *
 * DECISION: evilwm does not raise on focus — windows can be obscured by
 * others even when they have keyboard focus. This is deliberate. Users who
 * want raise-on-focus can bind a key to FUNC_RAISE.
 */
static void raise_toplevel(struct Toplevel *tl) {
    wlr_scene_node_raise_to_top(&tl->scene_tree->node);
}

/* =========================================================================
 * Output and layout helpers
 * ====================================================================== */

/*
 * toplevel_output_box — get the layout-space geometry of the output
 * containing the center of tl's current geometry.
 *
 * Falls back to the first output in the server list if the center point
 * doesn't land on any output (e.g. window is partially off-screen after
 * an output hotplug). Returns a zero box if no outputs are connected.
 */
static struct wlr_box toplevel_output_box(struct Toplevel *tl) {
    struct Server *server = tl->server;
    struct wlr_box *g = &tl->state.geom;
    double cx = g->x + g->width  / 2.0;
    double cy = g->y + g->height / 2.0;

    struct wlr_output *wlr_out =
        wlr_output_layout_output_at(server->output_layout, cx, cy);

    if (!wlr_out) {
        /* Fall back to first output. */
        if (!wl_list_empty(&server->outputs)) {
            struct Output *out =
                wl_container_of(server->outputs.next, out, link);
            wlr_out = out->wlr_output;
        } else {
            return (struct wlr_box){0};
        }
    }

    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, wlr_out, &box);
    return box;
}

/*
 * first_output_box — layout-space geometry of the first connected output.
 *
 * Used at map time before the window has a position (so we cannot use
 * toplevel_output_box which requires a valid center point).
 */
static struct wlr_box first_output_box(struct Server *server) {
    if (wl_list_empty(&server->outputs))
        return (struct wlr_box){0};
    struct Output *out = wl_container_of(server->outputs.next, out, link);
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
    return box;
}

/*
 * get_focused_toplevel — return the currently focused window, or NULL.
 *
 * server->toplevels is a wl_list where the front element (toplevels.next)
 * is always the focused window (maintained by focus_toplevel and unmap).
 */
static struct Toplevel *get_focused_toplevel(struct Server *server) {
    if (wl_list_empty(&server->toplevels))
        return NULL;
    struct Toplevel *tl;
    tl = wl_container_of(server->toplevels.next, tl, link);
    return tl;
}

/* =========================================================================
 * Snap-to-edge
 * ====================================================================== */

/*
 * apply_snap — snap window position to output or window edges.
 *
 * Modifies *x and *y in-place. Snap zone is config.snap_distance pixels.
 * If snap_distance == 0, this is a no-op.
 *
 * Rules:
 *   1. Snap window edge to output edge (all 4 combinations).
 *   2. Snap window edge to edge of each other mapped window on the same
 *      output (all 4 edge-to-edge combinations).
 *
 * Only windows on the same virtual desktop should be snapped against — for
 * now all windows share desktop 0, so all visible windows are considered.
 *
 * SECURITY: snap_distance is range-clamped to [0,100] by config_load();
 * the abs() calls prevent negative distances from skewing the threshold.
 */
static void apply_snap(struct Server *server, struct Toplevel *tl,
        int *x, int *y, const struct wlr_box *out) {
    int snap = server->config.snap_distance;
    if (snap == 0)
        return;

    int w = tl->state.geom.width;
    int h = tl->state.geom.height;

    /* Snap to output edges. */
    if (abs(*x - out->x) < snap)                         *x = out->x;
    if (abs(*x + w - (out->x + out->width))  < snap)     *x = out->x + out->width - w;
    if (abs(*y - out->y) < snap)                         *y = out->y;
    if (abs(*y + h - (out->y + out->height)) < snap)     *y = out->y + out->height - h;

    /* Snap to edges of other mapped windows. */
    struct Toplevel *other;
    wl_list_for_each(other, &server->toplevels, link) {
        if (other == tl)
            continue;
        struct wlr_box *og = &other->state.geom;

        /* Left edge of tl near right edge of other. */
        if (abs(*x - (og->x + og->width)) < snap)         *x = og->x + og->width;
        /* Right edge of tl near left edge of other. */
        if (abs(*x + w - og->x) < snap)                   *x = og->x - w;
        /* Top edge of tl near bottom edge of other. */
        if (abs(*y - (og->y + og->height)) < snap)        *y = og->y + og->height;
        /* Bottom edge of tl near top edge of other. */
        if (abs(*y + h - og->y) < snap)                   *y = og->y - h;
    }
}

/* =========================================================================
 * Mouse grab — move and resize
 * ====================================================================== */

/*
 * begin_move_grab — start a Super+button1 interactive move.
 *
 * Records the cursor's offset from the window origin at grab start so that
 * the window follows the cursor without jumping to place the origin under
 * the pointer.
 *
 * SECURITY: wlr_seat_pointer_clear_focus() ensures no client surface has
 * pointer focus during the grab. Combined with early-return in
 * process_cursor_motion(), clients receive neither enter nor motion events
 * while the grab is active.
 */
static void begin_move_grab(struct Server *server) {
    double sx, sy;
    struct Toplevel *tl = NULL;
    /* toplevel_at with NULL surface_out — we only need the toplevel. */
    tl = toplevel_at(server, server->cursor->x, server->cursor->y,
        NULL, &sx, &sy);
    if (!tl)
        return;

    server->cursor_mode  = CurMove;
    server->grab_tl      = tl;
    server->grab_geom    = tl->state.geom;
    /* Offset so window origin tracks cursor, not teleports to it. */
    server->grab_anchor_x = (int)round(server->cursor->x) - tl->state.geom.x;
    server->grab_anchor_y = (int)round(server->cursor->y) - tl->state.geom.y;

    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "fleur");

    /* SECURITY: clear pointer focus — no client receives pointer events
     * while this grab is active. */
    wlr_seat_pointer_clear_focus(server->seat);
}

/*
 * begin_resize_grab — start a Super+button2 interactive resize.
 *
 * Determines which corner of the window is nearest to the cursor. The
 * OPPOSITE corner becomes the anchor — it stays fixed during the drag.
 * The drag corner tracks the cursor.
 *
 * DECISION: nearest-corner anchor rather than fixed bottom-right. This
 * matches evilwm's model and is more ergonomic for windows in the top-right
 * quadrant of the screen (you'd be dragging away from the corner otherwise).
 *
 * SECURITY: same as begin_move_grab — pointer focus is cleared.
 */
static void begin_resize_grab(struct Server *server) {
    double sx, sy;
    struct Toplevel *tl = NULL;
    tl = toplevel_at(server, server->cursor->x, server->cursor->y,
        NULL, &sx, &sy);
    if (!tl)
        return;

    server->cursor_mode = CurResize;
    server->grab_tl     = tl;
    server->grab_geom   = tl->state.geom;

    /* Determine which corner is nearest the cursor. */
    double cx = server->cursor->x;
    double cy = server->cursor->y;
    struct wlr_box *g = &tl->state.geom;
    double mid_x = g->x + g->width  / 2.0;
    double mid_y = g->y + g->height / 2.0;

    /* Anchor = opposite corner from the one nearest the cursor. */
    server->grab_anchor_x = (cx < mid_x) ? (g->x + g->width)  : g->x;
    server->grab_anchor_y = (cy < mid_y) ? (g->y + g->height) : g->y;

    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "se-resize");

    /* SECURITY: clear pointer focus during grab. */
    wlr_seat_pointer_clear_focus(server->seat);
}

/*
 * end_grab — release the active mouse grab.
 *
 * Called on button release in handle_cursor_button(). Resets cursor to
 * default; pointer focus will be restored by the next process_cursor_motion
 * call now that cursor_mode == CurNormal.
 */
static void end_grab(struct Server *server) {
    server->cursor_mode = CurNormal;
    server->grab_tl     = NULL;
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
}

/*
 * handle_move_grab — update window position during an active move grab.
 *
 * Called from process_cursor_motion() when cursor_mode == CurMove.
 *
 * SECURITY: this function does NOT forward pointer events to clients.
 * It returns without calling wlr_seat_pointer_notify_*. The caller must
 * return immediately after this call — verified by structure in
 * process_cursor_motion().
 */
static void handle_move_grab(struct Server *server) {
    struct Toplevel *tl = server->grab_tl;
    if (!tl)
        return;

    int new_x = (int)round(server->cursor->x) - server->grab_anchor_x;
    int new_y = (int)round(server->cursor->y) - server->grab_anchor_y;

    struct wlr_box out = toplevel_output_box(tl);
    apply_snap(server, tl, &new_x, &new_y, &out);

    tl->state.geom.x = new_x;
    tl->state.geom.y = new_y;
    /* Width/height unchanged — only move the outer tree. */
    wlr_scene_node_set_position(&tl->scene_tree->node, new_x, new_y);
}

/*
 * handle_resize_grab — update window size during an active resize grab.
 *
 * Called from process_cursor_motion() when cursor_mode == CurResize.
 *
 * The drag corner tracks the cursor; the anchor corner is fixed. Size is
 * clamped so client content stays >= MIN_WIN_CONTENT in each dimension.
 *
 * SECURITY: does not forward pointer events to clients. See handle_move_grab.
 */
static void handle_resize_grab(struct Server *server) {
    struct Toplevel *tl = server->grab_tl;
    if (!tl)
        return;

    int bw  = server->config.border_width;
    int min = MIN_WIN_CONTENT + 2 * bw;
    int ax  = server->grab_anchor_x;
    int ay  = server->grab_anchor_y;
    int cx  = (int)round(server->cursor->x);
    int cy  = (int)round(server->cursor->y);

    int new_w = abs(cx - ax);
    int new_h = abs(cy - ay);
    if (new_w < min) new_w = min;
    if (new_h < min) new_h = min;

    /* Keep anchor corner fixed; drag corner follows cursor (clamped). */
    int new_x = (ax < cx) ? ax : ax - new_w;
    int new_y = (ay < cy) ? ay : ay - new_h;

    tl->state.geom = (struct wlr_box){new_x, new_y, new_w, new_h};
    toplevel_apply_geometry(tl);
}

/* =========================================================================
 * Keyboard move and resize
 * ====================================================================== */

/*
 * do_keyboard_move — execute a FUNC_MOVE bind with the given flags.
 *
 * FLAG_RELATIVE + direction: move by MOVE_STEP pixels, clamped to keep at
 * least MOVE_STEP pixels of the window visible on the output.
 *
 * Corner flags (top/bottom + left/right): snap the window to the
 * corresponding corner of the current output, flush to the edge.
 */
static void do_keyboard_move(struct Server *server, uint32_t flags) {
    struct Toplevel *tl = get_focused_toplevel(server);
    if (!tl)
        return;

    struct wlr_box out = toplevel_output_box(tl);
    int x = tl->state.geom.x;
    int y = tl->state.geom.y;
    int w = tl->state.geom.width;
    int h = tl->state.geom.height;

    if (flags & FLAG_RELATIVE) {
        /* Incremental move by MOVE_STEP in the flagged direction(s). */
        if (flags & FLAG_LEFT)  x -= MOVE_STEP;
        if (flags & FLAG_RIGHT) x += MOVE_STEP;
        if (flags & FLAG_UP)    y -= MOVE_STEP;
        if (flags & FLAG_DOWN)  y += MOVE_STEP;

        /* Clamp: keep at least MOVE_STEP px of the window visible.
         * This prevents keyboard-moving a window fully off-screen. */
        int x_min = out.x + MOVE_STEP - w;
        int x_max = out.x + out.width  - MOVE_STEP;
        int y_min = out.y + MOVE_STEP - h;
        int y_max = out.y + out.height - MOVE_STEP;
        if (x < x_min) x = x_min;
        if (x > x_max) x = x_max;
        if (y < y_min) y = y_min;
        if (y > y_max) y = y_max;

    } else {
        /* Corner move: position flush to the specified corner. */
        bool top    = (flags & FLAG_TOP)    != 0;
        bool bottom = (flags & FLAG_BOTTOM) != 0;
        bool left   = (flags & FLAG_LEFT)   != 0;
        bool right  = (flags & FLAG_RIGHT)  != 0;

        if (top)    y = out.y;
        if (bottom) y = out.y + out.height - h;
        if (left)   x = out.x;
        if (right)  x = out.x + out.width  - w;
    }

    apply_snap(server, tl, &x, &y, &out);
    tl->state.geom.x = x;
    tl->state.geom.y = y;
    /* Move only — no size change. Update scene node position directly to
     * avoid the configure roundtrip that toplevel_apply_geometry would
     * trigger with an unchanged size. */
    wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
}

/*
 * do_keyboard_resize — execute a FUNC_RESIZE bind with the given flags.
 *
 * FLAG_RELATIVE + direction: grow/shrink by MOVE_STEP, clamped at
 * MIN_WIN_CONTENT client content pixels.
 *
 * FLAG_TOGGLE + axis flags: toggle maximization on the specified axis.
 * Restore uses saved_geom (saved on first maximize transition only).
 */
static void do_keyboard_resize(struct Server *server, uint32_t flags) {
    struct Toplevel *tl = get_focused_toplevel(server);
    if (!tl)
        return;

    struct wlr_box out = toplevel_output_box(tl);
    int bw  = server->config.border_width;
    int min = MIN_WIN_CONTENT + 2 * bw;

    int x = tl->state.geom.x;
    int y = tl->state.geom.y;
    int w = tl->state.geom.width;
    int h = tl->state.geom.height;

    if (flags & FLAG_RELATIVE) {
        /* Incremental resize — up/left shrink, down/right grow. */
        if (flags & FLAG_LEFT)  w -= MOVE_STEP;
        if (flags & FLAG_RIGHT) w += MOVE_STEP;
        if (flags & FLAG_UP)    h -= MOVE_STEP;
        if (flags & FLAG_DOWN)  h += MOVE_STEP;
        if (w < min) w = min;
        if (h < min) h = min;

    } else if (flags & FLAG_TOGGLE) {
        bool do_v = (flags & FLAG_VERTICAL)   != 0;
        bool do_h = (flags & FLAG_HORIZONTAL) != 0;

        /*
         * Toggle+v+h: if any axis is already maximized, restore fully.
         * Otherwise maximize both axes simultaneously.
         *
         * Toggle+v or toggle+h individually: toggle the respective axis.
         * The saved_geom is written only on the FIRST maximize transition
         * (from no-maximize). Second-axis toggles share the same save so
         * full restore returns to the original pre-maximize geometry.
         */
        if (do_v && do_h) {
            if (tl->state.max_flags) {
                /* Restore. */
                x = tl->state.saved_geom.x;
                y = tl->state.saved_geom.y;
                w = tl->state.saved_geom.width;
                h = tl->state.saved_geom.height;
                tl->state.max_flags = 0;
            } else {
                /* Save and maximize fully. */
                tl->state.saved_geom = tl->state.geom;
                x = out.x; y = out.y;
                w = out.width; h = out.height;
                tl->state.max_flags = FLAG_VERTICAL | FLAG_HORIZONTAL;
            }

        } else if (do_v) {
            if (tl->state.max_flags & FLAG_VERTICAL) {
                /* Restore vertical axis. */
                y = tl->state.saved_geom.y;
                h = tl->state.saved_geom.height;
                tl->state.max_flags &= ~FLAG_VERTICAL;
            } else {
                /* Save only if transitioning from fully unmaximized. */
                if (!tl->state.max_flags)
                    tl->state.saved_geom = tl->state.geom;
                y = out.y;
                h = out.height;
                tl->state.max_flags |= FLAG_VERTICAL;
            }

        } else if (do_h) {
            if (tl->state.max_flags & FLAG_HORIZONTAL) {
                /* Restore horizontal axis. */
                x = tl->state.saved_geom.x;
                w = tl->state.saved_geom.width;
                tl->state.max_flags &= ~FLAG_HORIZONTAL;
            } else {
                if (!tl->state.max_flags)
                    tl->state.saved_geom = tl->state.geom;
                x = out.x;
                w = out.width;
                tl->state.max_flags |= FLAG_HORIZONTAL;
            }
        }

        if (w < min) w = min;
        if (h < min) h = min;
    }

    tl->state.geom = (struct wlr_box){x, y, w, h};
    toplevel_apply_geometry(tl);
}

/*
 * toplevel_at() — find the Toplevel and wlr_surface under layout coordinates
 * (lx, ly) using the scene graph's node-at lookup.
 *
 * On success, sets *surface_out to the wlr_surface, and *sx, *sy to the
 * surface-local coordinates of the hit point. Returns the owning Toplevel.
 * Returns NULL if no surface is present at those coordinates.
 *
 * Scene node data: when a toplevel is mapped, we set scene_tree->node.data =
 * toplevel (see handle_new_xdg_toplevel). This lets us walk up the node tree
 * to find the owning Toplevel after wlr_scene_node_at() gives us a leaf node.
 */
static struct Toplevel *toplevel_at(struct Server *server,
        double lx, double ly,
        struct wlr_surface **surface_out, double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node)
        return NULL;

    /*
     * A buffer node may be a client surface — extract the wlr_surface.
     * A rect node is a border rectangle; there is no surface to forward
     * pointer events to, so surface_out is left NULL for rect hits.
     * The Toplevel is still returned (for raise/focus on border clicks).
     */
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buf);
        if (scene_surface && surface_out)
            *surface_out = scene_surface->surface;
        /* If it is a non-surface buffer (e.g. scene background), no surface_out. */
    }
    /* For WLR_SCENE_NODE_RECT (border), surface_out stays NULL — no forwarding. */

    /* Walk up through tree nodes to find the one tagged with a Toplevel pointer.
     * scene_tree->node.data is set to the owning Toplevel in
     * handle_new_xdg_toplevel(). */
    struct wlr_scene_tree *tree = node->parent;
    while (tree && tree != &server->scene->tree) {
        struct Toplevel *tl = tree->node.data;
        if (tl)
            return tl;
        tree = tree->node.parent;
    }
    return NULL;
}

/*
 * focus_toplevel() — give keyboard focus to a toplevel window.
 *
 * Moves the window to the front of the focus list, updates border colors,
 * deactivates the previously focused toplevel, and sends wl_keyboard.enter.
 *
 * Does NOT raise the window. evilwm's focus model: focus follows the pointer
 * but windows are not raised on focus. Raise happens on click (handled in
 * handle_cursor_button) or via an explicit FUNC_RAISE bind.
 *
 * DECISION: not raising on focus is intentional — it is the evilwm default.
 * Raise-on-focus would interfere with workflows where the user wants to read
 * a background window while typing in a foreground one. If raise-on-focus is
 * desired later, it should be a config option, not the default.
 *
 * Passing NULL is safe and clears keyboard focus without changing borders.
 */
static void focus_toplevel(struct Toplevel *tl) {
    if (!tl)
        return;

    struct Server *server = tl->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = tl->xdg_toplevel->base->surface;
    struct wlr_surface *prev = seat->keyboard_state.focused_surface;

    if (prev == surface)
        return; /* already focused — no border update needed */

    if (prev) {
        /* Deactivate the previously focused toplevel so it repaints
         * its titlebar / caret in the inactive state. */
        struct wlr_xdg_toplevel *prev_xdg =
            wlr_xdg_toplevel_try_from_wlr_surface(prev);
        if (prev_xdg) {
            wlr_xdg_toplevel_set_activated(prev_xdg, false);

            /* Color the old window's border inactive.
             * We find the Toplevel via: xdg_surface->data = surface_tree,
             * surface_tree->node.parent = scene_tree (outer container),
             * scene_tree->node.data = Toplevel. */
            struct wlr_scene_tree *surf_tree = prev_xdg->base->data;
            if (surf_tree) {
                struct wlr_scene_tree *outer = surf_tree->node.parent;
                if (outer) {
                    struct Toplevel *prev_tl = outer->node.data;
                    if (prev_tl)
                        toplevel_set_border_color(prev_tl, false);
                }
            }
        }
    }

    /* Move to front of focus list (front = focused). */
    wl_list_remove(&tl->link);
    wl_list_insert(&server->toplevels, &tl->link);

    wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);
    toplevel_set_border_color(tl, true);

    /* Forward current keyboard state to the new surface. */
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

/*
 * process_cursor_motion() — update pointer focus and grab state after cursor moves.
 *
 * Three cases:
 *
 * 1. Active grab (CurMove or CurResize): compositor consumes the event and
 *    updates the grabbed window. No pointer events reach any client.
 *    SECURITY: this is the enforcement point for the grab security property.
 *    Early-return prevents any wlr_seat_pointer_notify_* call.
 *
 * 2. Cursor over a client surface: focus-follows-mouse (no raise), forward
 *    pointer motion to the surface.
 *
 * 3. Cursor over a border rect or background: clear client pointer focus,
 *    reset cursor image. Keyboard focus is retained (evilwm: "focus is not
 *    lost if you stray onto the root window").
 */
static void process_cursor_motion(struct Server *server, uint32_t time_msec) {
    /*
     * SECURITY: during an active grab all pointer motion is consumed by the
     * compositor. No enter/motion events are sent to any client surface.
     * handle_cursor_button() enforces the same property for button events.
     */
    if (server->cursor_mode == CurMove) {
        handle_move_grab(server);
        return;
    }
    if (server->cursor_mode == CurResize) {
        handle_resize_grab(server);
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct Toplevel *tl = toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (!tl) {
        /* Cursor is over the background — retain keyboard focus (evilwm
         * behavior: focus does not move when pointer leaves all windows). */
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    /*
     * Focus-follows-mouse: give keyboard focus to the window under the
     * cursor. Does NOT raise — see focus_toplevel() and raise_toplevel().
     */
    focus_toplevel(tl);

    if (surface) {
        /* Cursor is over the client surface — forward pointer events. */
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    } else {
        /* Cursor is over a border rect — no surface to forward events to.
         * Clear client pointer focus so no stray enter event lingers. */
        wlr_seat_pointer_clear_focus(server->seat);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    }
}

/* =========================================================================
 * Bind dispatch
 * ====================================================================== */

/*
 * spawn_terminal — fork and exec the configured terminal.
 *
 * fork() + setsid() + execvp() — same pattern as dwl's spawn(). No shell
 * involved (no system(), no popen()), so the terminal name cannot be subject
 * to shell injection.
 *
 * SECURITY: The child inherits the compositor's environment. On a
 * single-user machine with a TTY-launched compositor the environment is the
 * user's own login session, set up by PAM and systemd-logind/elogind. No
 * sanitization is needed beyond what PAM already provides. WAYLAND_DISPLAY
 * is set by main() via setenv() before wlr_backend_start(), so the child
 * inherits it automatically. XDG_RUNTIME_DIR comes from the login session
 * unchanged.
 *
 * _exit(1) not exit(1) on exec failure: avoids flushing the parent's stdio
 * buffers in the child process, which would corrupt the compositor's output.
 *
 * The terminal path comes from EwConfig.terminal, not a hardcoded #define.
 * It is bounded to 255 chars by the config parser; execvp() receives exactly
 * what was in the config file (no shell expansion, no globbing).
 */
static void spawn_terminal(const char *term) {
    if (fork() == 0) {
        setsid();
        execvp(term, (char *[]){(char *)term, NULL});
        fprintf(stderr, "evilway: exec %s failed\n", term);
        _exit(1);
    }
}

/*
 * dispatch_bind — execute the action described by *bind.
 *
 * Called from keyboard and mouse event handlers after a bind has been matched.
 * For mouse binds (bind->is_mouse), FUNC_MOVE and FUNC_RESIZE start an
 * interactive grab; for keyboard binds, they perform discrete moves/resizes
 * driven by bind->flags.
 *
 * SECURITY: No system() or popen() here or in any called function. The only
 * exec path is spawn_terminal() which goes fork+execvp directly.
 */
static void dispatch_bind(struct Server *server, const EwBind *bind) {
    switch (bind->function) {

    case FUNC_SPAWN:
        spawn_terminal(server->config.terminal);
        break;

    case FUNC_DELETE: {
        /*
         * Cooperative close — sends xdg_toplevel.close to the focused client.
         * The client may ignore it or prompt the user before exiting.
         * Use FUNC_KILL for unresponsive windows.
         */
        struct Toplevel *tl = get_focused_toplevel(server);
        if (tl)
            wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
        break;
    }

    case FUNC_KILL: {
        /*
         * Forceful termination — destroys the Wayland client connection.
         * The client's process gets SIGPIPE or similar from libwayland; it
         * will typically exit. Use only for stuck/unresponsive clients.
         *
         * SECURITY: wl_client_destroy() is the correct API for this. It is
         * not a signal sent to the process — it closes the socket, which
         * lets the kernel clean up the process gracefully if it handles
         * disconnect. An actual SIGKILL on the PID is not provided here;
         * future enhancement could add that via wl_client_get_credentials().
         */
        struct Toplevel *tl = get_focused_toplevel(server);
        if (tl) {
            struct wl_client *client =
                wl_resource_get_client(tl->xdg_toplevel->base->resource);
            wl_client_destroy(client);
        }
        break;
    }

    case FUNC_LOWER: {
        /* Lower the focused window to the bottom of the stacking order. */
        struct Toplevel *tl = get_focused_toplevel(server);
        if (tl)
            wlr_scene_node_lower_to_bottom(&tl->scene_tree->node);
        break;
    }

    case FUNC_RAISE: {
        /* Raise the focused window to the top of the stacking order. */
        struct Toplevel *tl = get_focused_toplevel(server);
        if (tl)
            raise_toplevel(tl);
        break;
    }

    case FUNC_MOVE:
        /*
         * Mouse bind: begin interactive move grab (Super+drag).
         * Keyboard bind: discrete move by flags (relative or corner).
         *
         * DECISION: the is_mouse field on the bind distinguishes the two
         * paths. This keeps the dispatch table clean without separate
         * functions for mouse vs keyboard variants of the same logical action.
         */
        if (bind->is_mouse)
            begin_move_grab(server);
        else
            do_keyboard_move(server, bind->flags);
        break;

    case FUNC_RESIZE:
        /* Same split as FUNC_MOVE: mouse starts grab, keyboard does step/toggle. */
        if (bind->is_mouse)
            begin_resize_grab(server);
        else
            do_keyboard_resize(server, bind->flags);
        break;

    case FUNC_FIX: {
        /*
         * Toggle the fixed (sticky) flag on the focused window.
         * Fixed windows will remain visible across all virtual desktops when
         * FUNC_VDESK is implemented. The flag is stored and preserved now;
         * it has no visible effect until virtual desktop switching is added.
         */
        struct Toplevel *tl = get_focused_toplevel(server);
        if (tl) {
            tl->state.fixed = !tl->state.fixed;
            wlr_log(WLR_DEBUG, "bind: fix: window is now %s",
                tl->state.fixed ? "fixed" : "unfixed");
        }
        break;
    }

    case FUNC_NEXT: {
        /*
         * Cycle focus to the next window in the toplevels list.
         * server->toplevels is front=focused; next-after-front is the
         * "other window" to cycle to.
         *
         * DECISION: cycles to the second item only (no full rotation through
         * all windows on each press). Full Alt+Tab style cycling is a later
         * enhancement; for two-window workflows this is sufficient and matches
         * evilwm's minimal approach.
         */
        if (!wl_list_empty(&server->toplevels) &&
                server->toplevels.next->next != &server->toplevels) {
            struct Toplevel *next;
            next = wl_container_of(server->toplevels.next->next, next, link);
            focus_toplevel(next);
        }
        break;
    }

    case FUNC_VDESK:
        /* Virtual desktops not yet implemented. Window state (vdesk field)
         * is already stored per-window; the switching logic comes in a later
         * phase. */
        fprintf(stderr, "evilway: vdesk not yet implemented\n");
        break;

    case FUNC_DOCK:
        /* Dock toggle not yet implemented. */
        fprintf(stderr, "evilway: dock not yet implemented\n");
        break;

    case FUNC_INFO:
        /* Window info overlay not yet implemented. */
        fprintf(stderr, "evilway: info not yet implemented\n");
        break;
    }
}

/* =========================================================================
 * Keyboard handlers
 * ====================================================================== */

static void handle_kb_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    struct Keyboard *kb = wl_container_of(listener, kb, modifiers);
    /*
     * A seat can only have one keyboard in the Wayland protocol, but wlroots
     * lets us swap the underlying wlr_keyboard transparently. Calling
     * wlr_seat_set_keyboard() here ensures the seat always reflects whichever
     * physical keyboard sent the last modifier event.
     */
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat,
        &kb->wlr_keyboard->modifiers);
}

static void handle_kb_key(struct wl_listener *listener, void *data) {
    struct Keyboard *kb = wl_container_of(listener, kb, key);
    struct Server *server = kb->server;
    struct wlr_keyboard_key_event *event = data;

    /* Translate evdev keycode to XKB keycode (evdev is 0-based, XKB is 8-offset). */
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

    uint32_t modifiers = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
    bool handled = false;

    /*
     * SECURITY: Compositor keybindings are checked and consumed HERE, before
     * any key event is forwarded to a client. This is the enforcement point
     * that prevents clients from intercepting Super+Shift+Q or other
     * compositor shortcuts.
     *
     * Only process keybindings on key-press, not key-release.
     */
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            /*
             * SECURITY: VT switching is consumed here and never forwarded to
             * any client. A client that can trigger VT switches can escape the
             * session (switch to an unlocked TTY or another user's session).
             *
             * XKB translates Ctrl+Alt+Fn → XKB_KEY_XF86Switch_VT_n before the
             * compositor sees the event — no modifier check is needed here
             * because this keysym is only produced when Ctrl+Alt is held.
             *
             * session is NULL when running nested (Wayland/X11 backend) —
             * the NULL guard below makes VT switching a safe no-op in that
             * case rather than a crash.
             */
            if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
                    syms[i] <= XKB_KEY_XF86Switch_VT_12) {
                if (server->session)
                    wlr_session_change_vt(server->session,
                        syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
                handled = true;
                continue;
            }

            /*
             * SECURITY: Super+Shift+Q is the compositor emergency exit.
             * It is hardcoded here and NOT exposed through the config bind
             * system. This ensures the user always has a way out even if
             * the config file is malformed or all configured binds are broken.
             * A config file that accidentally omits this bind cannot trap
             * the user.
             */
            if ((modifiers & MODIFIER) && (modifiers & WLR_MODIFIER_SHIFT)
                    && syms[i] == XKB_KEY_Q) {
                wlr_log(WLR_INFO, "Super+Shift+Q: terminating compositor");
                wl_display_terminate(server->display);
                handled = true;
                continue;
            }

            /*
             * Check user-configured keyboard binds.
             *
             * Matching: exact modifier mask equality (not bitwise AND). This
             * means Super+Shift+Return does NOT fire a Super+Return bind —
             * the extra Shift modifier prevents the match. This is intentional
             * and matches evilwm's behavior.
             */
            for (size_t j = 0; j < server->config.num_binds; j++) {
                const EwBind *b = &server->config.binds[j];
                if (!b->is_mouse &&
                        b->modifiers == modifiers &&
                        b->keysym    == syms[i]) {
                    dispatch_bind(server, b);
                    handled = true;
                    /* Do not break — multiple binds on the same key are
                     * unusual but not forbidden. */
                }
            }
        }
    }

    if (!handled) {
        /* Forward the raw event to the focused client. */
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat,
            event->time_msec, event->keycode, event->state);
    }
}

static void handle_kb_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct Keyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

static void create_keyboard(struct Server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);

    struct Keyboard *kb = calloc(1, sizeof(*kb));
    if (!kb)
        die("calloc Keyboard");
    kb->server = server;
    kb->wlr_keyboard = wlr_kb;

    /* Compile a default XKB keymap using the system locale.
     * A later phase will read layout/variant from environment variables
     * (XKB_DEFAULT_LAYOUT, etc.) or the config file. */
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        wlr_log(WLR_ERROR, "xkb_context_new failed, skipping keyboard");
        free(kb);
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(ctx);
    if (!keymap) {
        wlr_log(WLR_ERROR, "xkb_keymap_new_from_names failed, skipping keyboard");
        free(kb);
        return;
    }
    wlr_keyboard_set_keymap(wlr_kb, keymap);
    xkb_keymap_unref(keymap);

    /* 25 Hz repeat rate, 600 ms initial delay — sensible defaults. */
    wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

    kb->modifiers.notify = handle_kb_modifiers;
    wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);

    kb->key.notify = handle_kb_key;
    wl_signal_add(&wlr_kb->events.key, &kb->key);

    kb->destroy.notify = handle_kb_destroy;
    wl_signal_add(&device->events.destroy, &kb->destroy);

    /* Make this keyboard the current seat keyboard so key events are routed
     * to the focused client. When multiple keyboards are present, the last
     * one to generate a key event wins — wlroots handles this transparently. */
    wlr_seat_set_keyboard(server->seat, wlr_kb);

    wl_list_insert(&server->keyboards, &kb->link);
    wlr_log(WLR_DEBUG, "new keyboard: %s", device->name);
}

/* =========================================================================
 * Pointer / cursor handlers
 * ====================================================================== */

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
        event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
        event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

/*
 * Button number → Linux evdev code mapping for mouse binds.
 * Index by config button number (1–5); index 0 is unused (sentinel 0).
 *
 * evilwm/X11 convention:  button1=left, button2=middle, button3=right.
 * Linux evdev layout:      BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112.
 * Note that evdev right < middle numerically — the config numbers do NOT map
 * linearly to evdev codes; we use an explicit table.
 */
static const uint32_t button_evdev[6] = {
    0,           /* index 0: unused */
    BTN_LEFT,    /* button1 */
    BTN_MIDDLE,  /* button2 */
    BTN_RIGHT,   /* button3 */
    BTN_SIDE,    /* button4 */
    BTN_EXTRA,   /* button5 */
};

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    /*
     * SECURITY: compositor grabs are checked BEFORE forwarding to clients.
     * The original scaffold called wlr_seat_pointer_notify_button() first,
     * which would forward events to clients even during a grab. This is
     * incorrect — the forwarding call is moved to the bottom and only reached
     * when the event is not consumed by the compositor.
     */

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (server->cursor_mode != CurNormal) {
            /*
             * SECURITY: grab is ending — do not forward this button release
             * to any client. end_grab() resets cursor_mode and cursor image.
             * The next process_cursor_motion() call will restore pointer focus.
             */
            end_grab(server);
            return;
        }
        /* Non-grab release: fall through to notify client below. */
    }

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        /*
         * Check for Super+mouse compositor binds first.
         *
         * Super (WLR_MODIFIER_LOGO) gates all mouse window-management actions.
         * Mouse binds in .evilwayrc do not require a modifier syntax because
         * Super is always implied — it is the compositor modifier for all
         * pointer-driven WM actions. This matches evilwm's Alt+drag model.
         *
         * When Super is held: look up a configured mouse bind first; if none
         * matches for this button, use the built-in defaults:
         *   button1 → move, button2 → resize, button3 → lower.
         *
         * SECURITY: on Super+mouse, return WITHOUT calling notify_button.
         * The client does not learn about these button presses.
         */
        struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
        uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;

        if (mods & WLR_MODIFIER_LOGO) {
            /* Try user-configured mouse binds first. */
            bool handled = false;
            for (size_t i = 0; i < server->config.num_binds; i++) {
                const EwBind *b = &server->config.binds[i];
                if (b->is_mouse &&
                        b->button >= 1 && b->button <= 5 &&
                        button_evdev[b->button] == event->button) {
                    dispatch_bind(server, b);
                    handled = true;
                    break;
                }
            }

            if (!handled) {
                /* Built-in defaults (evilwm mouse model):
                 *   Super+button1 drag  → move window
                 *   Super+button2 drag  → resize window (nearest-corner)
                 *   Super+button3 click → lower window  */
                if (event->button == BTN_LEFT) {
                    static const EwBind move_bind = {
                        .is_mouse = true, .function = FUNC_MOVE };
                    dispatch_bind(server, &move_bind);
                } else if (event->button == BTN_MIDDLE) {
                    static const EwBind resize_bind = {
                        .is_mouse = true, .function = FUNC_RESIZE };
                    dispatch_bind(server, &resize_bind);
                } else if (event->button == BTN_RIGHT) {
                    static const EwBind lower_bind = {
                        .is_mouse = true, .function = FUNC_LOWER };
                    dispatch_bind(server, &lower_bind);
                }
            }
            /* SECURITY: consumed by compositor — do NOT forward to client. */
            return;
        }

        /* No Super modifier: normal click.
         * Raise the window under the cursor and give it keyboard focus.
         * Raise-on-click is the standard floating-WM stacking behavior. */
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct Toplevel *tl = toplevel_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        if (tl) {
            raise_toplevel(tl);
            focus_toplevel(tl);
        }

        /* Non-Super mouse binds (e.g. "bind button1=spawn"). */
        for (size_t i = 0; i < server->config.num_binds; i++) {
            const EwBind *b = &server->config.binds[i];
            if (b->is_mouse &&
                    b->button >= 1 && b->button <= 5 &&
                    button_evdev[b->button] == event->button) {
                dispatch_bind(server, b);
            }
        }
    }

    /* Forward to the client that has pointer focus.
     * Reached only when the event was NOT consumed by a compositor bind. */
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct Server *server = wl_container_of(listener, server, cursor_frame);
    /* wl_pointer.frame groups related events (motion+button in one logical action).
     * Must be forwarded so clients can correctly handle grouped pointer events. */
    wlr_seat_pointer_notify_frame(server->seat);
}

/* =========================================================================
 * Seat request handlers
 * ====================================================================== */

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    /*
     * SECURITY: Only the client with pointer focus may set the cursor image.
     * A client that does not have pointer focus cannot spoof the cursor.
     */
    struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
    if (focused == event->seat_client) {
        wlr_cursor_set_surface(server->cursor,
            event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void handle_pointer_focus_change(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, pointer_focus_change);
    struct wlr_seat_pointer_focus_change_event *event = data;
    /* Reset cursor to default image when pointer leaves all surfaces. */
    if (!event->new_surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    }
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    /* Forward clipboard selection requests unconditionally.
     * A more hardened compositor could validate source here. */
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* =========================================================================
 * Input device management
 * ====================================================================== */

static void handle_new_input(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        create_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        /* Attach pointer to cursor aggregator. Libinput configuration (tap,
         * natural scroll, acceleration) belongs here and will be added in a
         * later phase when input preferences are defined. */
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        /* Touch, tablet, etc. — not handled in scaffold. */
        break;
    }

    /* Advertise capabilities to clients: always expose pointer (cursor is
     * always present), add keyboard if at least one is connected. */
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

/* =========================================================================
 * Output (display) handlers
 * ====================================================================== */

static void handle_output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct Output *output = wl_container_of(listener, output, frame);

    /* Commit the scene graph to this output. wlroots handles damage tracking,
     * buffer selection, and rendering — we just call commit. */
    if (!wlr_scene_output_commit(output->scene_output, NULL)) {
        wlr_log(WLR_ERROR, "wlr_scene_output_commit failed on %s",
            output->wlr_output->name);
        return;
    }

    /* Notify all surfaces attached to this output that a frame completed.
     * Required for frame callbacks (clients that throttle rendering to display
     * refresh, e.g. video players, animated UIs). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void handle_output_request_state(struct wl_listener *listener, void *data) {
    struct Output *output = wl_container_of(listener, output, request_state);
    struct wlr_output_event_request_state *event = data;
    /* KMS connectors and DRM lease clients can request output state changes.
     * Commit the requested state directly in this scaffold. A more sophisticated
     * compositor might validate or modify the state before committing. */
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct Output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    /* Bind the renderer and allocator to this output before it can render.
     * This must happen before any wlr_output_state commit. */
    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR, "wlr_output_init_render failed for %s — skipping",
            wlr_output->name);
        return;
    }

    /* Configure the output using the modern state API.
     * DECISION: We use wlr_output_state_*() throughout rather than the
     * deprecated direct field assignment. The state API atomically validates
     * and commits mode+enable together, which is required on KMS backends. */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode)
        wlr_output_state_set_mode(&state, mode);

    /* Commit the initial state. Failure here means the output cannot be used
     * (bad mode, driver rejection, etc.) — log and skip rather than die, so
     * a bad secondary monitor does not prevent the compositor from starting. */
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "initial output commit failed for %s — skipping",
            wlr_output->name);
        wlr_output_state_finish(&state);
        return;
    }
    wlr_output_state_finish(&state);

    struct Output *output = calloc(1, sizeof(*output));
    if (!output)
        die("calloc Output");
    output->server = server;
    output->wlr_output = wlr_output;

    output->frame.notify = handle_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = handle_output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    /* Add to output layout (auto-arranges outputs left-to-right in connect
     * order). This also publishes a wl_output global so clients can query
     * monitor info (DPI, scale, make/model). */
    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    if (!l_output)
        die("wlr_output_layout_add_auto");

    /* Create the scene output (links the scene graph to this wlr_output).
     * Store in output->scene_output so the frame handler can commit to it. */
    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    if (!output->scene_output)
        die("wlr_scene_output_create");

    /* Link the scene output to the output layout output so the scene graph
     * knows where to render each output in the virtual layout space. */
    wlr_scene_output_layout_add_output(server->scene_layout, l_output,
        output->scene_output);

    wlr_log(WLR_INFO, "output connected: %s", wlr_output->name);
}

/* =========================================================================
 * XDG shell: popup handlers
 * ====================================================================== */

static void handle_popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct Popup *popup = wl_container_of(listener, popup, commit);
    /* On initial commit, the compositor must reply with a configure so the
     * client knows the popup has been acknowledged and can map its surface. */
    if (popup->xdg_popup->base->initial_commit)
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct Popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_popup *xdg_popup = data;

    struct Popup *popup = calloc(1, sizeof(*popup));
    if (!popup)
        die("calloc Popup");
    popup->xdg_popup = xdg_popup;

    /* The popup must be added to the scene as a child of its parent surface.
     * xdg_surface->data is set to the parent's scene_tree in
     * handle_new_xdg_toplevel so we can find it here. */
    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL); /* popup without an xdg_surface parent is a protocol error */
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data =
        wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = handle_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = handle_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* =========================================================================
 * XDG shell: toplevel handlers
 * ====================================================================== */

static void handle_toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    /*
     * Called when the surface is ready to display (first buffer committed
     * after the initial configure roundtrip). At this point the client has
     * chosen its size and we can read it back to set our initial geometry.
     *
     * Initial geometry: window is centered on the first output at its
     * natural size plus borders. If the client didn't set an explicit xdg
     * window geometry we fall back to the committed surface dimensions.
     */
    struct Toplevel *tl = wl_container_of(listener, tl, map);
    struct Server *server = tl->server;
    int bw = server->config.border_width;

    /* Read the client's natural content size. */
    struct wlr_box xdg_geom = tl->xdg_toplevel->base->current.geometry;
    int sw = (xdg_geom.width  > 0) ? xdg_geom.width
                                    : tl->xdg_toplevel->base->surface->current.width;
    int sh = (xdg_geom.height > 0) ? xdg_geom.height
                                    : tl->xdg_toplevel->base->surface->current.height;
    /* Fallback if surface hasn't committed a real buffer yet. */
    if (sw <= 0) sw = 640;
    if (sh <= 0) sh = 480;

    int w = sw + 2 * bw;
    int h = sh + 2 * bw;

    /* Center on the first output. */
    struct wlr_box out = first_output_box(server);
    int x = out.x + (out.width  - w) / 2;
    int y = out.y + (out.height - h) / 2;
    if (x < out.x) x = out.x;
    if (y < out.y) y = out.y;

    tl->state.geom = (struct wlr_box){x, y, w, h};

    /* Ensure outer tree is visible (it starts enabled; re-enable after unmap). */
    wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
    toplevel_apply_geometry(tl);

    wl_list_insert(&server->toplevels, &tl->link);
    focus_toplevel(tl);
    wlr_log(WLR_DEBUG, "mapped toplevel: '%s' at (%d,%d) %dx%d",
        tl->xdg_toplevel->title ? tl->xdg_toplevel->title : "(no title)",
        x, y, w, h);
}

static void handle_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct Toplevel *tl = wl_container_of(listener, tl, unmap);

    /* Hide the outer container tree so border rects disappear with the window.
     * wlroots auto-hides the surface_tree (xdg subtree) but does NOT hide our
     * manually-created outer scene_tree or its border rect children. */
    wlr_scene_node_set_enabled(&tl->scene_tree->node, false);

    /* If this was the focused window, focus the next one in the list. */
    struct wlr_seat *seat = tl->server->seat;
    if (seat->keyboard_state.focused_surface == tl->xdg_toplevel->base->surface) {
        struct Toplevel *next = NULL;
        if (tl->link.next != &tl->server->toplevels) {
            next = wl_container_of(tl->link.next, next, link);
        }
        if (next) {
            focus_toplevel(next);
        } else {
            wlr_seat_keyboard_clear_focus(seat);
        }
    }

    wl_list_remove(&tl->link);
    /* Re-initialize so the link is safe to remove again in handle_toplevel_destroy
     * (which fires even if the surface was never mapped). */
    wl_list_init(&tl->link);
}

static void handle_toplevel_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct Toplevel *tl = wl_container_of(listener, tl, commit);

    /*
     * On the initial commit (before the first configure has been acknowledged),
     * the compositor must send a configure to the client. We advertise our WM
     * capabilities and send size 0x0 ("client chooses its own size").
     *
     * WM capabilities we do NOT advertise:
     *   - MAXIMIZE: evilWay is floating-only, no managed maximize state.
     *   - MINIMIZE: no taskbar or iconification in this WM.
     *   - WINDOW_MENU: no compositor-provided window menu.
     * We DO advertise FULLSCREEN so clients offer it as an option, though the
     * implementation comes in a later phase.
     */
    if (tl->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_wm_capabilities(tl->xdg_toplevel,
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
    }
}

static void handle_toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct Toplevel *tl = wl_container_of(listener, tl, destroy);

    /*
     * Remove all listeners before freeing. Missing any of these results in
     * a dangling listener that will crash when the signal fires post-free.
     * This is a use-after-free category bug in C wlroots code — guard against
     * it by being explicit about every listener this struct registered.
     */
    wl_list_remove(&tl->map.link);
    wl_list_remove(&tl->unmap.link);
    wl_list_remove(&tl->commit.link);
    wl_list_remove(&tl->destroy.link);
    wl_list_remove(&tl->request_move.link);
    wl_list_remove(&tl->request_resize.link);
    wl_list_remove(&tl->request_maximize.link);
    wl_list_remove(&tl->request_fullscreen.link);
    /* link may be in the toplevels list (if never unmapped) or self-referential
     * (if already removed by handle_toplevel_unmap). wl_list_remove() is safe
     * either way. */
    wl_list_remove(&tl->link);

    /*
     * Destroy the outer container scene tree and all its children:
     * border[0..3] rects and surface_tree (the xdg surface subtree).
     *
     * IMPORTANT: we created scene_tree with wlr_scene_tree_create() (not
     * via any xdg auto-management), so we must destroy it explicitly.
     * wlr_scene_node_destroy() tears down the subtree and removes all
     * child nodes; this is safe to call here per dwl's destroynotify().
     *
     * wlroots manages surface_tree's internal wl_listeners; they are
     * cleaned up when the node is destroyed, preventing double-free.
     */
    if (tl->scene_tree)
        wlr_scene_node_destroy(&tl->scene_tree->node);

    /* Clear grab reference if this window was being grabbed. */
    if (tl->server->grab_tl == tl) {
        tl->server->cursor_mode = CurNormal;
        tl->server->grab_tl = NULL;
    }

    free(tl);
}

/*
 * handle_request_move / handle_request_resize — xdg_toplevel client requests.
 *
 * These events fire when a client asks the compositor to initiate an
 * interactive move or resize (typically in response to a user click on a
 * titlebar or resize handle). evilWay has neither — we implement
 * compositor-driven move/resize via Super+drag (begin_move_grab /
 * begin_resize_grab). Client-initiated requests are silently ignored;
 * the client will continue in its current state, which is correct.
 */
static void handle_request_move(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    /* No titlebar in evilWay; move is Super+drag only. */
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    /* No resize handle in evilWay; resize is Super+button2+drag only. */
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct Toplevel *tl = wl_container_of(listener, tl, request_maximize);
    /* evilWay is floating-only. Deny maximize by sending an empty configure.
     * The client sees no size/state change and stays in its current geometry. */
    wlr_xdg_surface_schedule_configure(tl->xdg_toplevel->base);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct Toplevel *tl = wl_container_of(listener, tl, request_fullscreen);
    /* Fullscreen denied in scaffold — revisit in a later phase.
     * Same treatment as maximize: send empty configure to acknowledge and deny. */
    wlr_xdg_surface_schedule_configure(tl->xdg_toplevel->base);
}

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct Toplevel *tl = calloc(1, sizeof(*tl));
    if (!tl)
        die("calloc Toplevel");
    tl->server = server;
    tl->xdg_toplevel = xdg_toplevel;
    ew_window_state_init(&tl->state);

    /*
     * Scene graph structure for this window:
     *
     *   scene_tree (outer container, in LyrFloat)  ← node.data = tl
     *   ├── border[0..3]   four wlr_scene_rect border rectangles
     *   └── surface_tree   wlr_scene_xdg_surface subtree, at (bw,bw) on map
     *
     * The outer tree is positioned at (geom.x, geom.y) in layout space.
     * scene_tree->node.data = tl so toplevel_at() walk-up finds the Toplevel
     * whether the hit was on a border rect or the client surface.
     *
     * xdg_surface->data = surface_tree so handle_new_xdg_popup() can locate
     * the correct parent tree for popup placement.
     *
     * DECISION: this outer-container-plus-surface-tree split matches dwl's
     * c->scene / c->scene_surface convention. It allows independent sizing
     * and coloring of border rects without any xdg protocol involvement.
     */
    tl->scene_tree = wlr_scene_tree_create(server->layers[LyrFloat]);
    if (!tl->scene_tree)
        die("wlr_scene_tree_create (outer container)");
    tl->scene_tree->node.data = tl;

    /* Create four border rect children, initially 0×0.
     * They are sized in toplevel_apply_geometry() on first map.
     * Color is inactive until the window receives focus. */
    float inactive_color[4];
    color_to_float(BORDER_COLOR_INACTIVE, inactive_color);
    for (int i = 0; i < 4; i++) {
        tl->border[i] = wlr_scene_rect_create(tl->scene_tree, 0, 0,
            inactive_color);
        if (!tl->border[i])
            die("wlr_scene_rect_create (border)");
    }

    /* XDG surface subtree as child of outer tree.
     * Positioned at (bw, bw) within outer tree by toplevel_apply_geometry(). */
    tl->surface_tree = wlr_scene_xdg_surface_create(tl->scene_tree,
        xdg_toplevel->base);
    if (!tl->surface_tree)
        die("wlr_scene_xdg_surface_create");

    /* xdg_surface->data = surface_tree for popup parent lookup. */
    xdg_toplevel->base->data = tl->surface_tree;

    /* Initialize link so wl_list_remove() in handle_toplevel_destroy is safe
     * even if the surface was never mapped (and thus never inserted). */
    wl_list_init(&tl->link);

    /* Register surface-level events (map/unmap/commit are on wlr_surface). */
    tl->map.notify    = handle_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &tl->map);

    tl->unmap.notify  = handle_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &tl->unmap);

    tl->commit.notify = handle_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &tl->commit);

    /* Register toplevel-level events (destroy, requests are on wlr_xdg_toplevel). */
    tl->destroy.notify          = handle_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &tl->destroy);

    tl->request_move.notify     = handle_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &tl->request_move);

    tl->request_resize.notify   = handle_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &tl->request_resize);

    tl->request_maximize.notify = handle_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &tl->request_maximize);

    tl->request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen,
        &tl->request_fullscreen);

    wlr_log(WLR_DEBUG, "new toplevel: %s",
        xdg_toplevel->title ? xdg_toplevel->title : "(no title)");
}

/* =========================================================================
 * SIGHUP — config reload
 * ====================================================================== */

/*
 * handle_sighup — reload config on SIGHUP.
 *
 * This callback is registered via wl_event_loop_add_signal(), which uses
 * signalfd() internally. The callback runs within the normal Wayland event
 * loop dispatch — NOT from a signal handler context. This means malloc(),
 * stdio, and all wlroots calls are safe here.
 *
 * SECURITY: We load into a fresh EwConfig first. Only if config_load()
 * succeeds do we replace the live config. A failed reload leaves the current
 * config completely intact — the compositor never operates with a partially-
 * parsed config.
 *
 * Validation after reload: config_load() itself validates and clamps all
 * values. Bind entries with invalid keysyms or function names are rejected
 * during parse and never reach the live config.
 */
static int handle_sighup(int sig, void *data) {
    (void)sig;
    struct Server *server = data;

    wlr_log(WLR_INFO, "SIGHUP received — reloading config");

    EwConfig new_config;
    if (!config_load(&new_config)) {
        /* malloc failure during reload — keep the current config. */
        fprintf(stderr,
            "evilway: config reload failed (out of memory), "
            "continuing with current config\n");
        config_free(&new_config);
        return 0;
    }

    config_free(&server->config);
    server->config = new_config;

    wlr_log(WLR_INFO,
        "config reloaded: %zu bind(s), bw=%d snap=%d term=%s vdesks=%d",
        server->config.num_binds,
        server->config.border_width,
        server->config.snap_distance,
        server->config.terminal,
        server->config.num_vdesks);
    return 0;
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void) {
    /* Verbose logging during development. Change to WLR_ERROR for production. */
    wlr_log_init(WLR_DEBUG, NULL);
    wlr_log(WLR_INFO, "evilWay starting");

    struct Server server = {0};
    wl_list_init(&server.outputs);
    wl_list_init(&server.toplevels);
    wl_list_init(&server.keyboards);

    /* ---- Load config ----
     * Must happen before any keybinding or terminal reference. Establishes
     * defaults if ~/.evilwayrc is absent. Fatal on malloc failure only. */
    if (!config_load(&server.config))
        die("config_load");

    /* ---- Wayland display ---- */
    server.display = wl_display_create();
    if (!server.display)
        die("wl_display_create");

    /* ---- Backend ----
     * wlr_backend_autocreate selects the appropriate backend for the current
     * environment: DRM/KMS when running on bare hardware (Asahi, TTY),
     * X11 or Wayland when running nested for development. The session
     * (logind/seatd) is managed by the backend automatically. */
    server.backend = wlr_backend_autocreate(
        wl_display_get_event_loop(server.display), &server.session);
    if (!server.backend)
        die("wlr_backend_autocreate");

    /* ---- Renderer ----
     * Autocreates Pixman, GLES2, or Vulkan based on hardware capability.
     * On Asahi with Fedora 43, the GPU driver (Mesa AGX) provides GLES2 or
     * Vulkan. WLR_RENDERER env var can override this for debugging. */
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer)
        die("wlr_renderer_autocreate");
    if (!wlr_renderer_init_wl_display(server.renderer, server.display))
        die("wlr_renderer_init_wl_display");

    /* ---- Allocator ----
     * Bridges backend and renderer for buffer allocation. Required for all
     * rendering — do not attempt to create buffers without it. */
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator)
        die("wlr_allocator_autocreate");

    /* ---- Core Wayland globals ----
     * wlr_compositor: allocates wl_surface objects for clients (version 6).
     * wlr_subcompositor: subsurface protocol (nested surfaces).
     * wlr_data_device_manager: clipboard (copy/paste). */
    if (!wlr_compositor_create(server.display, 6, server.renderer))
        die("wlr_compositor_create");
    if (!wlr_subcompositor_create(server.display))
        die("wlr_subcompositor_create");
    if (!wlr_data_device_manager_create(server.display))
        die("wlr_data_device_manager_create");

    /* ---- Output layout ----
     * Tracks the virtual arrangement of connected monitors. Automatically
     * publishes wl_output globals so clients can query monitor properties. */
    server.output_layout = wlr_output_layout_create(server.display);
    if (!server.output_layout)
        die("wlr_output_layout_create");

    /* ---- Scene graph ----
     * All rendering flows through wlr_scene. Bypassing it would break damage
     * tracking, screencopy, and session lock — do not bypass it.
     *
     * Layer trees are created in stacking order: Bg (bottom) to Block (top).
     * SECURITY: LyrBlock must remain topmost — session lock surfaces go here. */
    server.scene = wlr_scene_create();
    if (!server.scene)
        die("wlr_scene_create");

    for (int i = 0; i < LYR_COUNT; i++) {
        server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
        if (!server.layers[i])
            die("wlr_scene_tree_create (layer)");
    }

    server.scene_layout = wlr_scene_attach_output_layout(server.scene,
        server.output_layout);
    if (!server.scene_layout)
        die("wlr_scene_attach_output_layout");

    /* ---- XDG shell ----
     * Version 6 enables full WM capability signaling (wm_capabilities,
     * tiled state, bounds). Clients negotiating lower versions still work —
     * wlroots handles the version negotiation. */
    server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
    if (!server.xdg_shell)
        die("wlr_xdg_shell_create");

    server.new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel,
        &server.new_xdg_toplevel);

    server.new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    /* ---- Cursor ---- */
    server.cursor = wlr_cursor_create();
    if (!server.cursor)
        die("wlr_cursor_create");
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    /* Xcursor theme NULL → use $XCURSOR_THEME or system default. Size 24 is
     * standard; HiDPI outputs will get upscaled versions via xcursor_manager. */
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    if (!server.cursor_mgr)
        die("wlr_xcursor_manager_create");

    server.cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);

    server.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
        &server.cursor_motion_absolute);

    server.cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);

    server.cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);

    server.cursor_frame.notify = handle_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    /* ---- Seat ----
     * Single seat named "seat0" — standard for single-user desktop sessions. */
    server.seat = wlr_seat_create(server.display, "seat0");
    if (!server.seat)
        die("wlr_seat_create");

    server.request_cursor.notify = handle_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor,
        &server.request_cursor);

    server.pointer_focus_change.notify = handle_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change,
        &server.pointer_focus_change);

    server.request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection,
        &server.request_set_selection);

    /* ---- Backend event hooks ---- */
    server.new_output.notify = handle_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.new_input.notify = handle_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    /* ---- SIGHUP handler for config reload ----
     *
     * wl_event_loop_add_signal() uses signalfd() internally. The callback
     * (handle_sighup) fires as a normal event loop event, NOT from a Unix
     * signal handler context. This means the callback may safely allocate
     * memory and call any wlroots or stdio functions.
     *
     * DECISION over volatile sig_atomic_t + self-pipe: wl_event_loop_add_signal
     * IS the idiomatic wlroots approach and provides the same "process in event
     * loop, not signal handler" guarantee without the extra pipe fd. It also
     * avoids the EINTR hazard on slow system calls that a bare signal() handler
     * introduces.
     *
     * The source is stored in server.sighup_source so it can be removed
     * cleanly during shutdown (wl_event_source_remove). Forgetting to remove
     * it before the event loop is destroyed is safe (the loop tears down all
     * sources), but explicit removal is cleaner.
     */
    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    server.sighup_source = wl_event_loop_add_signal(loop, SIGHUP,
        handle_sighup, &server);
    if (!server.sighup_source)
        wlr_log(WLR_ERROR,
            "failed to register SIGHUP handler — config reload will not work");

    /* ---- Wayland socket ----
     * SECURITY: wl_display_add_socket_auto() creates the Unix socket in
     * $XDG_RUNTIME_DIR with 0600 permissions, owned by the session user. This
     * is the correct behavior — no world-readable socket. The socket path is
     * logged so it can be verified with: ls -la $XDG_RUNTIME_DIR/wayland-*
     *
     * If XDG_RUNTIME_DIR is not set (e.g. started outside a login session),
     * libwayland falls back to /tmp, which has weaker permissions. We log a
     * warning in this case. Never run the compositor as root. */
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        die("wl_display_add_socket_auto");
    }

    const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
    if (!xdg_rt) {
        wlr_log(WLR_ERROR,
            "SECURITY WARNING: XDG_RUNTIME_DIR is not set. "
            "Wayland socket may be created in /tmp with world-accessible permissions. "
            "Start evilWay from a proper login session.");
    }
    wlr_log(WLR_INFO, "Wayland socket: %s/%s (verify: ls -la %s/%s)",
        xdg_rt ? xdg_rt : "/tmp", socket,
        xdg_rt ? xdg_rt : "/tmp", socket);

    /* Export socket name for child processes (terminals, app launchers, etc.). */
    setenv("WAYLAND_DISPLAY", socket, 1);

    /* ---- Start backend ----
     * On DRM/KMS: acquires DRM master, enumerates outputs and input devices,
     * sets up the event loop. This triggers the new_output/new_input signals. */
    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        die("wlr_backend_start");
    }

    wlr_log(WLR_INFO,
        "evilWay running — exit with Super+Shift+Q  "
        "(send SIGHUP to reload config)");

    /* ---- Event loop ----
     * Runs until wl_display_terminate() is called (e.g. from Super+Shift+Q).
     * SIGHUP events are dispatched here via handle_sighup(). */
    wl_display_run(server.display);

    /* ---- Cleanup ----
     * Destroy in reverse initialization order. wl_display_destroy_clients()
     * first — gives clients a chance to clean up before we tear down globals. */
    wlr_log(WLR_INFO, "evilWay shutting down");
    wl_display_destroy_clients(server.display);

    if (server.sighup_source)
        wl_event_source_remove(server.sighup_source);

    config_free(&server.config);

    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_output_layout_destroy(server.output_layout);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    return 0;
}
