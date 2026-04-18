/* vdesk.c — virtual desktop management
 *
 * Implements evilwm's "virtual screens" — flat numbered desktops arranged
 * in a configurable columns×rows grid for directional navigation.
 *
 * Fixed views are visible on all desktops.
 */

#include "evilway.h"

/* Total number of virtual desktops */
static int total_vdesks(struct ew_server *server) {
	return server->config.vdesks_cols * server->config.vdesks_rows;
}

/* Convert flat index to col,row and back */
static void vdesk_to_grid(struct ew_server *server, int vd, int *col,
                          int *row) {
	int cols = server->config.vdesks_cols;
	*col = vd % cols;
	*row = vd / cols;
}

static int grid_to_vdesk(struct ew_server *server, int col, int row) {
	return row * server->config.vdesks_cols + col;
}

/* ── Update view visibility for current vdesk ───────────────────────── */

void vdesk_update_visibility(struct ew_server *server) {
	struct ew_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->scene_tree) continue;
		bool visible = view->fixed ||
		               view->vdesk == server->current_vdesk;
		wlr_scene_node_set_enabled(&view->scene_tree->node, visible);
	}
}

/* ── Switch to absolute vdesk number ────────────────────────────────── */

void vdesk_switch(struct ew_server *server, int vdesk) {
	int total = total_vdesks(server);
	if (vdesk < 0 || vdesk >= total) return;
	if (vdesk == server->current_vdesk) return;

	server->prev_vdesk = server->current_vdesk;
	server->current_vdesk = vdesk;

	wlr_log(WLR_INFO, "vdesk: %d → %d", server->prev_vdesk, vdesk);

	vdesk_update_visibility(server);

	/* Focus a view on the new desktop if possible */
	struct ew_view *v;
	bool focused = false;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && !v->is_dock &&
		    (v->fixed || v->vdesk == server->current_vdesk)) {
			view_focus(server, v);
			focused = true;
			break;
		}
	}
	if (!focused) {
		input_focus_view(server, NULL);
	}
}

/* ── Switch relative to current position in grid ────────────────────── */

void vdesk_switch_relative(struct ew_server *server, unsigned flags) {
	int col, row;
	vdesk_to_grid(server, server->current_vdesk, &col, &row);

	int cols = server->config.vdesks_cols;
	int rows = server->config.vdesks_rows;

	if (flags & FL_LEFT)  col = (col - 1 + cols) % cols;
	if (flags & FL_RIGHT) col = (col + 1) % cols;
	if (flags & FL_UP)    row = (row - 1 + rows) % rows;
	if (flags & FL_DOWN)  row = (row + 1) % rows;

	int target = grid_to_vdesk(server, col, row);
	vdesk_switch(server, target);
}

/* ── Toggle: switch to previously active vdesk ──────────────────────── */

void vdesk_switch_toggle(struct ew_server *server) {
	vdesk_switch(server, server->prev_vdesk);
}
