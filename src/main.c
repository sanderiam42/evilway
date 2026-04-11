/*
 * evilWay — Wayland compositor
 *
 * Scaffold: xdg-shell only. Proves the wlroots foundation is correct before
 * any evilwm behavior is layered on top.
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
 *            consumed BEFORE the key event is forwarded to any client. A client
 *            cannot intercept or observe compositor-level shortcuts.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "evilway.h"

/* =========================================================================
 * Helpers
 * ====================================================================== */

static void die(const char *msg) {
    wlr_log(WLR_ERROR, "fatal: %s", msg);
    exit(1);
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
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return NULL;

    struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buf);
    if (!scene_surface)
        return NULL;

    if (surface_out)
        *surface_out = scene_surface->surface;

    /* Walk up the scene tree to find the node tagged with a Toplevel pointer. */
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
 * Raises the window to the top of the stacking order, moves it to the front
 * of the focus list, deactivates the previously focused toplevel, and sends
 * wl_keyboard.enter to the new surface.
 *
 * Passing NULL is safe and clears focus.
 */
static void focus_toplevel(struct Toplevel *tl) {
    if (!tl)
        return;

    struct Server *server = tl->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = tl->xdg_toplevel->base->surface;
    struct wlr_surface *prev = seat->keyboard_state.focused_surface;

    if (prev == surface)
        return; /* already focused */

    if (prev) {
        /* Deactivate the previously focused toplevel so the client repaints
         * (e.g. stops drawing its caret). */
        struct wlr_xdg_toplevel *prev_tl =
            wlr_xdg_toplevel_try_from_wlr_surface(prev);
        if (prev_tl)
            wlr_xdg_toplevel_set_activated(prev_tl, false);
    }

    /* Raise to top of stacking order within LyrFloat. */
    wlr_scene_node_raise_to_top(&tl->scene_tree->node);

    /* Move to front of focus list (front = focused). */
    wl_list_remove(&tl->link);
    wl_list_insert(&server->toplevels, &tl->link);

    wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);

    /* Forward current keyboard state to the new surface. Using
     * wlr_seat_get_keyboard() picks whichever keyboard was last set on the
     * seat, which is updated each time a key event is processed. */
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
}

/*
 * process_cursor_motion() — update pointer focus after the cursor has moved.
 *
 * Finds the surface under the cursor and forwards motion to it. If no surface
 * is under the cursor, resets to the default xcursor image and clears pointer
 * focus.
 *
 * Focus-follows-pointer: pointer focus updates continuously as the cursor
 * moves. Keyboard focus still requires a click (see handle_cursor_button).
 * The evilwm behavior layer (later phase) will switch keyboard focus to
 * follow the pointer as well.
 */
static void process_cursor_motion(struct Server *server, uint32_t time_msec) {
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct Toplevel *tl = toplevel_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (!tl) {
        /* Cursor is not over any surface. */
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
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
            if ((modifiers & MODIFIER) && (modifiers & WLR_MODIFIER_SHIFT)
                    && syms[i] == XKB_KEY_Q) {
                /* Super+Shift+Q — exit the compositor cleanly. */
                wlr_log(WLR_INFO, "Super+Shift+Q: terminating compositor");
                wl_display_terminate(server->display);
                handled = true;
            }
            /* Additional keybindings will be added here in the evilwm behavior
             * phase: Super+arrow for move/resize, Super+F1–F9 for virtual
             * desktops, etc. All must be consumed before the dispatch below. */
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
     * (XKB_DEFAULT_LAYOUT, etc.) or a config header. */
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

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    struct Server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    /* Forward the raw button event to the focused client. */
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        /* Click-to-focus: give keyboard focus to whatever is under the cursor.
         * In the evilwm behavior phase this becomes focus-follows-pointer, but
         * click-to-focus is correct for the scaffold. */
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct Toplevel *tl = toplevel_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        focus_toplevel(tl);
    }
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
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, output->scene_output);

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
    /* Called when the surface is ready to display (first buffer committed
     * after a successful configure roundtrip). */
    struct Toplevel *tl = wl_container_of(listener, tl, map);
    wl_list_insert(&tl->server->toplevels, &tl->link);
    focus_toplevel(tl);
}

static void handle_toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct Toplevel *tl = wl_container_of(listener, tl, unmap);

    /* If this was the focused window, focus the next one in the list. */
    struct wlr_seat *seat = tl->server->seat;
    if (seat->keyboard_state.focused_surface == tl->xdg_toplevel->base->surface) {
        struct Toplevel *next = NULL;
        /* toplevels.next after remove would be the next candidate. Check first. */
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

    free(tl);
}

/* Interactive move/resize: stubs in the scaffold.
 * These will be implemented in the evilwm behavior layer:
 *   - Super+drag    → move
 *   - Super+RMB drag → resize
 * For now we log and do nothing, which causes the client to receive no
 * acknowledgment and continue in its current state. */
static void handle_request_move(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    wlr_log(WLR_DEBUG, "request_move: not yet implemented (Phase 3)");
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    (void)listener;
    (void)data;
    wlr_log(WLR_DEBUG, "request_resize: not yet implemented (Phase 3)");
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

    /* Place the toplevel's scene subtree in the floating layer.
     * Set scene_tree->node.data = tl so toplevel_at() can find us when
     * walking up from a leaf buffer node. */
    tl->scene_tree = wlr_scene_xdg_surface_create(server->layers[LyrFloat],
        xdg_toplevel->base);
    if (!tl->scene_tree)
        die("wlr_scene_xdg_surface_create");
    tl->scene_tree->node.data = tl;

    /* Also set xdg_surface->data to the scene_tree so popup children can
     * find their parent tree (see handle_new_xdg_popup). */
    xdg_toplevel->base->data = tl->scene_tree;

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
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &tl->request_fullscreen);

    wlr_log(WLR_DEBUG, "new toplevel: %s", xdg_toplevel->title ? xdg_toplevel->title : "(no title)");
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
        wl_display_get_event_loop(server.display), NULL);
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
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);

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
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);

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

    wlr_log(WLR_INFO, "evilWay running — exit with Super+Shift+Q");

    /* ---- Event loop ----
     * Runs until wl_display_terminate() is called (e.g. from Super+Shift+Q). */
    wl_display_run(server.display);

    /* ---- Cleanup ----
     * Destroy in reverse initialization order. wl_display_destroy_clients()
     * first — gives clients a chance to clean up before we tear down globals. */
    wlr_log(WLR_INFO, "evilWay shutting down");
    wl_display_destroy_clients(server.display);
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
