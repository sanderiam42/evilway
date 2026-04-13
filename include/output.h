/* include/output.h — wlr-output-management-v1 protocol interface
 *
 * Declares the two functions that main.c calls into from the output
 * management subsystem. All protocol implementation lives in src/output.c.
 *
 * The wlr-output-management-v1 protocol allows external tools (wlr-randr,
 * kanshi) to query and configure outputs — resolution, refresh rate, scale,
 * position, orientation — without any of that logic living inside the
 * compositor. evilWay implements the server side; the ecosystem tooling
 * does the policy decisions.
 *
 * Target: wlroots-0.19 (Fedora 43: 0.19.2-1.fc43).
 */
#ifndef OUTPUT_H
#define OUTPUT_H

/* Forward declaration — including evilway.h is the caller's responsibility. */
struct Server;

/*
 * output_mgmt_init — create the wlr-output-management-v1 manager global
 * and wire apply/test event listeners.
 *
 * Call once during compositor initialization, after wl_display and
 * output_layout have been created. No outputs need to be connected yet.
 *
 * Non-fatal if the manager cannot be created (logs an error and returns;
 * compositor continues without output management support).
 */
void output_mgmt_init(struct Server *server);

/*
 * output_mgmt_notify_layout_changed — broadcast current output layout to clients.
 *
 * Call after:
 *   - A new output is hotplugged (from handle_new_output in main.c)
 *   - A successful apply (handled internally by output.c)
 *
 * Builds a snapshot of the current output layout and passes it to the manager,
 * which broadcasts it to all connected wlr-randr / kanshi clients.
 *
 * No-op if output_mgmt_init() failed (server->output_manager is NULL).
 */
void output_mgmt_notify_layout_changed(struct Server *server);

#endif /* OUTPUT_H */
