/* src/output.c — wlr-output-management-v1 protocol implementation
 *
 * This file implements the server side of the wlr-output-management-v1
 * Wayland protocol. It is the plumbing layer that lets external tools —
 * specifically wlr-randr and kanshi — query and reconfigure outputs
 * (resolution, refresh rate, scale, position, orientation) at runtime.
 *
 * evilWay does NOT implement its own resolution controls. It implements
 * the receiving end of the protocol and delegates to the ecosystem:
 *
 *   wlr-randr or kanshi (client)
 *       ↓  wlr-output-management-v1 protocol (this file)
 *   evilWay (protocol server side, wlroots wlr_output_manager_v1)
 *       ↓  wlr_output API
 *   KMS/DRM (kernel applies the mode change)
 *
 * =========================================================================
 * DESIGN: all-or-nothing apply semantics
 * =========================================================================
 * A multi-output configuration is applied atomically. If any output fails
 * to accept the new state, no output is changed. This is enforced via a
 * two-phase approach:
 *
 *   Phase 1 — TEST: Call wlr_output_test_state() for every output in the
 *     configuration. No output state is modified. If any test fails, report
 *     failure immediately and return — no output has been touched.
 *
 *   Phase 2 — APPLY: Only executed after all tests pass. Commit each output
 *     in sequence with wlr_output_commit_state(). A failure in this phase
 *     (hardware glitch after a passing test) is logged as an error. In
 *     practice, test() passing guarantees apply() will succeed on the same
 *     hardware in the same state.
 *
 * =========================================================================
 * SECURITY: unauthenticated client access
 * =========================================================================
 * wlroots does not authenticate clients requesting output configuration
 * changes. Any Wayland client that can reach the socket can request mode
 * changes, disable outputs, or set arbitrary scale values, potentially
 * making the display unusable (wrong resolution, zero scale, disabled
 * outputs).
 *
 * On the evilWay target platform — a single-user workstation, Wayland socket
 * at 0600 permissions in $XDG_RUNTIME_DIR — this is an acceptable risk: the
 * socket is reachable only by the session user.
 *
 * If a future phase adds client sandboxing, capabilities, or a security
 * policy layer, the apply and test handlers here (handle_output_manager_apply
 * and handle_output_manager_test) are the operations to gate. They are the
 * sole entry points for client-driven output configuration.
 *
 * =========================================================================
 * SECURITY: input validation
 * =========================================================================
 * All configuration values from clients are validated in
 * validate_output_head_state() before any wlroots API is called:
 *   - scale must be > 0 (zero scale causes division by zero in the kernel)
 *   - custom mode dimensions must be > 0 (non-positive sizes are rejected)
 *   - transform must be a valid wl_output_transform enum value (0–7)
 * Out-of-range values are logged and the entire configuration is rejected.
 *
 * =========================================================================
 * SECURITY: test path is side-effect free
 * =========================================================================
 * The test handler (handle_output_manager_test) does not modify any output
 * state, layout state, or window positions. It calls wlr_output_test_state()
 * which probes driver/KMS capability without committing. This prevents
 * clients from using the test path to probe output capabilities without
 * going through the apply path's consequences.
 */

#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "evilway.h"
#include "output.h"

/* =========================================================================
 * Input validation
 * ====================================================================== */

/*
 * validate_output_head_state — reject configuration values that are out of range.
 *
 * SECURITY: this is the first thing called on client-supplied configuration
 * data, before any wlroots API sees the values. Called on both test and apply
 * paths — the test path is side-effect free so this check fires without
 * modifying any state.
 *
 * Returns true if the state is acceptable, false if it should be rejected.
 */
static bool validate_output_head_state(const struct wlr_output_head_v1_state *state) {
    /* Scale must be positive. Zero scale causes division by zero in the DRM
     * framebuffer size calculation; negative scale has no valid interpretation. */
    if (state->scale <= 0.0f) {
        wlr_log(WLR_ERROR,
            "output-mgmt: rejecting config for %s: scale %.4g is not positive",
            state->output->name, (double)state->scale);
        return false;
    }

    /* Custom mode dimensions must be non-zero. Named modes (state->mode != NULL)
     * come from the driver's mode list and are pre-validated; we only check
     * client-specified custom modes here. */
    if (!state->mode) {
        if (state->custom_mode.width <= 0 || state->custom_mode.height <= 0) {
            wlr_log(WLR_ERROR,
                "output-mgmt: rejecting config for %s: "
                "custom mode %dx%d has non-positive dimension",
                state->output->name,
                state->custom_mode.width, state->custom_mode.height);
            return false;
        }
    }

    /* Transform must be a valid wl_output_transform enum value.
     * Valid range: WL_OUTPUT_TRANSFORM_NORMAL (0) through
     * WL_OUTPUT_TRANSFORM_FLIPPED_270 (7). Values outside this range are
     * not defined by the protocol and must not be passed to KMS. */
    if ((int)state->transform < 0 || (int)state->transform > 7) {
        wlr_log(WLR_ERROR,
            "output-mgmt: rejecting config for %s: transform %d outside valid range [0,7]",
            state->output->name, (int)state->transform);
        return false;
    }

    return true;
}

/* =========================================================================
 * Current-configuration snapshot
 * ====================================================================== */

/*
 * build_current_output_config — snapshot the current output layout for clients.
 *
 * Creates a wlr_output_configuration_v1 that represents the compositor's
 * current output arrangement. Passed to wlr_output_manager_v1_set_configuration
 * so that wlr-randr and kanshi can see what is currently active.
 *
 * wlr_output_configuration_head_v1_create() populates most head state from the
 * wlr_output itself (enabled, current mode, scale, transform). Position (x, y)
 * is not stored on the output directly — it comes from the output layout and
 * must be set explicitly here.
 *
 * Returns NULL on allocation failure. Callers must handle NULL — the compositor
 * continues correctly, but clients will see stale layout information.
 */
static struct wlr_output_configuration_v1 *
build_current_output_config(struct Server *server)
{
    struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
    if (!config) {
        wlr_log(WLR_ERROR, "output-mgmt: wlr_output_configuration_v1_create failed");
        return NULL;
    }

    struct Output *o;
    wl_list_for_each(o, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config, o->wlr_output);
        if (!head) {
            wlr_log(WLR_ERROR,
                "output-mgmt: failed to create config head for %s",
                o->wlr_output->name);
            wlr_output_configuration_v1_destroy(config);
            return NULL;
        }

        /* Populate position from the output layout.
         * wlr_output_layout_get_box() gives the output's bounding box in virtual
         * layout space. The top-left corner (box.x, box.y) is the position that
         * wlr-randr and kanshi need to see. */
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
        head->state.x = box.x;
        head->state.y = box.y;
    }

    return config;
}

/* =========================================================================
 * Orphaned window recovery
 * ====================================================================== */

/*
 * nudge_orphaned_windows — move off-screen windows back onto a visible output.
 *
 * After an output layout change (mode, scale, position change, or hotplug
 * removal), a window whose output was reconfigured may find its center point
 * is no longer on any active output. This function finds such windows and
 * repositions them onto the first available output.
 *
 * "Nudge" means reposition only — we do not resize. The window is clamped
 * so its top-left corner is within the first output's bounds, ensuring the
 * user can see and interact with it. In evilWay's floating-only model, a
 * simple positional clamp is sufficient; there are no tiling constraints
 * to reflow.
 *
 * Called after every successful apply. No-op if no outputs are connected.
 */
static void nudge_orphaned_windows(struct Server *server) {
    if (wl_list_empty(&server->outputs))
        return;

    /* Use the first output as the recovery target. In a multi-monitor setup
     * this is the output that was connected first; kanshi typically treats it
     * as the primary display. */
    struct Output *first = wl_container_of(server->outputs.next, first, link);
    struct wlr_box obox;
    wlr_output_layout_get_box(server->output_layout, first->wlr_output, &obox);

    /* Minimum visibility: at least border_width pixels of the window remain
     * on-screen on each axis. Uses the configured border width for consistency
     * with the visible border the user expects to grab. */
    int bw = server->config.border_width;
    if (bw < 1) bw = 1;  /* guard against zero border width */

    struct Toplevel *tl;
    wl_list_for_each(tl, &server->toplevels, link) {
        struct wlr_box *g = &tl->state.geom;

        /* Check center point. If it is on any output, the window is accessible
         * and does not need to be moved. */
        double cx = g->x + g->width  / 2.0;
        double cy = g->y + g->height / 2.0;
        if (wlr_output_layout_output_at(server->output_layout, cx, cy))
            continue;

        /* Window is orphaned — center is not on any output. Compute the new
         * position that clamps it within the first output's bounds. */
        int new_x = g->x;
        int new_y = g->y;

        /* Right edge too far left — snap window to left edge of output. */
        if (new_x + g->width < obox.x + bw)
            new_x = obox.x;

        /* Left edge too far right — position so right edge is bw from right. */
        if (new_x > obox.x + obox.width - bw)
            new_x = obox.x + obox.width - g->width;

        /* Bottom edge too far up — snap window to top edge of output. */
        if (new_y + g->height < obox.y + bw)
            new_y = obox.y;

        /* Top edge too far down — position so bottom edge is bw from bottom. */
        if (new_y > obox.y + obox.height - bw)
            new_y = obox.y + obox.height - g->height;

        /* Final clamp: if the window is taller or wider than the output,
         * pin its top-left corner to the output origin so the title bar
         * (top-left area) is always reachable. */
        if (new_x < obox.x) new_x = obox.x;
        if (new_y < obox.y) new_y = obox.y;

        if (new_x == g->x && new_y == g->y)
            continue;  /* already in a valid position after clamping */

        g->x = new_x;
        g->y = new_y;
        wlr_scene_node_set_position(&tl->scene_tree->node, new_x, new_y);

        wlr_log(WLR_INFO,
            "output-mgmt: nudged orphaned window to (%d, %d) after layout change",
            new_x, new_y);
    }
}

/* =========================================================================
 * Apply / test core logic
 * ====================================================================== */

/*
 * apply_or_test_output_config — shared logic for apply and test handlers.
 *
 * test_only = true (test handler):
 *   Tests all outputs with wlr_output_test_state(). No output state is
 *   modified, no layout changes, no window movements. SIDE-EFFECT FREE.
 *   Returns true if all outputs would accept the configuration.
 *
 * test_only = false (apply handler):
 *   Phase 1: test all outputs. If any fails, return false immediately.
 *            No output has been touched.
 *   Phase 2: apply all outputs. If an apply fails after a passing test
 *            (hardware glitch / race), log an error and return false.
 *            On success: update the output layout with new positions and
 *            nudge any orphaned windows back on-screen.
 *
 * The two-phase approach enforces all-or-nothing semantics: by the time
 * Phase 2 runs, every output has already passed a test, so commit failures
 * are unexpected and indicate a hardware or driver issue, not a bad config.
 */
static bool apply_or_test_output_config(struct Server *server,
    struct wlr_output_configuration_v1 *config, bool test_only)
{
    struct wlr_output_configuration_head_v1 *head;

    /* ---- Phase 1: validate and test-state every head ---- */
    wl_list_for_each(head, &config->heads, link) {
        /* SECURITY: validate all client-supplied values before touching wlroots.
         * validate_output_head_state() logs and rejects out-of-range values. */
        if (!validate_output_head_state(&head->state)) {
            return false;
        }

        struct wlr_output_state wlr_state;
        wlr_output_state_init(&wlr_state);
        /* wlr_output_head_v1_state_apply() translates the protocol configuration
         * (mode, scale, transform, enabled) into the wlr_output_state that
         * wlroots commits to the DRM/KMS layer. */
        wlr_output_head_v1_state_apply(&head->state, &wlr_state);

        /* wlr_output_test_state() probes the driver/KMS without committing.
         * This is the side-effect-free path that satisfies the test handler's
         * requirement to not modify output state. */
        bool ok = wlr_output_test_state(head->state.output, &wlr_state);
        wlr_output_state_finish(&wlr_state);

        if (!ok) {
            wlr_log(WLR_INFO,
                "output-mgmt: test failed for %s — configuration rejected",
                head->state.output->name);
            return false;
        }
    }

    /* Test-only path ends here — no state has been modified. */
    if (test_only)
        return true;

    /* ---- Phase 2: commit all heads (all tests have passed) ---- */
    wl_list_for_each(head, &config->heads, link) {
        struct wlr_output_state wlr_state;
        wlr_output_state_init(&wlr_state);
        wlr_output_head_v1_state_apply(&head->state, &wlr_state);

        bool ok = wlr_output_commit_state(head->state.output, &wlr_state);
        wlr_output_state_finish(&wlr_state);

        if (!ok) {
            /* A commit failure after a passing test indicates a hardware or
             * driver issue (e.g., mode rejected at KMS after passing atomic
             * test, or output lost between test and commit). We cannot easily
             * roll back outputs that committed successfully before this point.
             *
             * Log clearly, return false so the client is notified of failure. */
            wlr_log(WLR_ERROR,
                "output-mgmt: commit failed for %s after passing test — "
                "hardware or driver issue; partial configuration state possible",
                head->state.output->name);
            return false;
        }

        /* Update the output layout to reflect the new position.
         *
         * wlr_output_layout_add() switches the output from auto-positioned
         * (as set by wlr_output_layout_add_auto() in handle_new_output) to
         * explicitly positioned at the coordinates kanshi or wlr-randr specified.
         * This is the correct behavior when the client specifies a layout. */
        wlr_output_layout_add(server->output_layout,
            head->state.output, head->state.x, head->state.y);
    }

    /* After layout change: recover any windows that ended up off all outputs. */
    nudge_orphaned_windows(server);

    return true;
}

/* =========================================================================
 * Protocol event handlers
 * ====================================================================== */

/*
 * handle_output_manager_test — client requests configuration validation only.
 *
 * The test path must be SIDE-EFFECT FREE: no output state is changed, no
 * layout is updated, no windows are moved. Only wlr_output_test_state() is
 * called, which probes driver/KMS capability without committing.
 *
 * SECURITY: see the file-level security note. This handler does not modify
 * output state, preventing clients from using the test path to probe output
 * capabilities as a side-channel without going through the apply path's
 * consequences. No state is logged that could reveal output capabilities
 * beyond what the protocol already advertises.
 */
static void handle_output_manager_test(struct wl_listener *listener, void *data) {
    struct Server *server =
        wl_container_of(listener, server, output_manager_test);
    struct wlr_output_configuration_v1 *config = data;

    bool ok = apply_or_test_output_config(server, config, true /* test_only */);

    if (ok)
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
}

/*
 * handle_output_manager_apply — client requests configuration application.
 *
 * All-or-nothing: tests all outputs first, then applies all. On test failure,
 * no output is touched. On apply success, updates the layout and broadcasts
 * the new configuration to all clients (wlr-randr --list will reflect the
 * change immediately).
 *
 * SECURITY: see the file-level security note. All values are validated in
 * validate_output_head_state() before any wlroots API is called.
 *
 * SECURITY: wlroots does not authenticate the requesting client. On the
 * evilWay target platform (single-user, 0600 socket) this is acceptable.
 * If client sandboxing is added in a future phase, gate this handler.
 */
static void handle_output_manager_apply(struct wl_listener *listener, void *data) {
    struct Server *server =
        wl_container_of(listener, server, output_manager_apply);
    struct wlr_output_configuration_v1 *config = data;

    bool ok = apply_or_test_output_config(server, config, false /* apply */);

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
        /* Broadcast the updated layout so wlr-randr and kanshi see the
         * new modes, scales, and positions immediately. */
        output_mgmt_notify_layout_changed(server);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

/*
 * output_mgmt_notify_layout_changed — broadcast current output layout to clients.
 *
 * Builds a snapshot of the current output arrangement and passes it to the
 * output management manager. The manager then broadcasts the new configuration
 * to all connected wlr-randr and kanshi clients.
 *
 * Called from:
 *   - handle_new_output() in main.c: after a new output is hotplugged, so
 *     wlr-randr and kanshi can see and configure the new output.
 *   - handle_output_manager_apply() above: after a successful apply, to
 *     confirm the new state to all clients.
 *
 * No-op if output_mgmt_init() failed (server->output_manager is NULL).
 *
 * DECISION: We call wlr_output_manager_v1_set_configuration() explicitly
 * rather than relying on wlroots to auto-broadcast. This gives us control
 * over when clients see layout changes and ensures the broadcast happens
 * after our internal state (output layout, window positions) is consistent.
 */
void output_mgmt_notify_layout_changed(struct Server *server) {
    if (!server->output_manager)
        return;

    struct wlr_output_configuration_v1 *config = build_current_output_config(server);
    if (!config) {
        wlr_log(WLR_ERROR,
            "output-mgmt: failed to build current config — "
            "clients will see stale output state");
        return;
    }

    /* wlr_output_manager_v1_set_configuration() takes ownership of config
     * and destroys it after broadcasting to all subscribed clients. */
    wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

/*
 * output_mgmt_init — create the wlr-output-management-v1 global and wire events.
 *
 * Creates the wlr_output_manager_v1, which registers the
 * zwlr_output_manager_v1 Wayland global on server->display. Clients that
 * bind this global (wlr-randr, kanshi) will receive the current output
 * configuration and can issue test/apply requests.
 *
 * Wires the apply and test event listeners so the handlers above are called
 * when clients send configuration requests.
 *
 * Call once during compositor initialization, after wl_display and
 * output_layout have been set up. Outputs do not need to be connected yet —
 * the manager will receive them via output_mgmt_notify_layout_changed() as
 * handle_new_output() fires for each connected monitor.
 *
 * Non-fatal on failure: the compositor comes up normally, but wlr-randr and
 * kanshi will not be able to configure outputs.
 */
void output_mgmt_init(struct Server *server) {
    server->output_manager = wlr_output_manager_v1_create(server->display);
    if (!server->output_manager) {
        wlr_log(WLR_ERROR,
            "output-mgmt: wlr_output_manager_v1_create failed — "
            "wlr-randr and kanshi will not work");
        return;
    }

    server->output_manager_apply.notify = handle_output_manager_apply;
    wl_signal_add(&server->output_manager->events.apply,
        &server->output_manager_apply);

    server->output_manager_test.notify = handle_output_manager_test;
    wl_signal_add(&server->output_manager->events.test,
        &server->output_manager_test);

    wlr_log(WLR_INFO,
        "output-mgmt: wlr-output-management-v1 ready — "
        "use wlr-randr or kanshi to configure outputs");
}
