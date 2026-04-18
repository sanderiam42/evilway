/* window.c — view (client window) management
 *
 * Handles view lifecycle: create on new xdg_toplevel, map, unmap, destroy.
 * Draws borders using wlr_scene_rect nodes around each surface.
 * Applies app matching rules on map.
 */

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "evilway.h"

/* ── View lookup at position ────────────────────────────────────────── */

struct ew_view *view_at(struct ew_server *server, double lx, double ly,
                        struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) return NULL;

	*surface = scene_surface->surface;

	/* Walk up the scene tree to find our view's tree node */
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) {
		tree = tree->node.parent;
	}
	if (!tree) return NULL;

	return tree->node.data;
}

/* ── Position + border update ───────────────────────────────────────── */

void view_set_position(struct ew_view *view, int x, int y) {
	view->x = x;
	view->y = y;
	if (view->scene_tree) {
		int bw = view->server->config.border_width;
		wlr_scene_node_set_position(&view->scene_tree->node,
			x - bw, y - bw);
	}
}

void view_update_borders(struct ew_view *view) {
	struct ew_server *server = view->server;
	int bw = server->config.border_width;

	if (bw <= 0) {
		for (int i = 0; i < BORDER_COUNT; i++) {
			if (view->border[i])
				wlr_scene_node_set_enabled(&view->border[i]->node, false);
		}
		return;
	}

	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
	int w = geo.width;
	int h = geo.height;

	/* Pick color based on focus and fixed state */
	float *color;
	if (view == server->focused_view)
		color = view->fixed ? server->config.fc : server->config.fg;
	else
		color = server->config.bg;

	/* Border layout (all relative to scene_tree origin):
	 * scene_tree is positioned at (view->x - bw, view->y - bw)
	 * The xdg surface child is at offset (bw, bw) within the tree.
	 *
	 * TOP:    x=0, y=0, w=w+2*bw, h=bw
	 * BOTTOM: x=0, y=bw+h, w=w+2*bw, h=bw
	 * LEFT:   x=0, y=bw, w=bw, h=h
	 * RIGHT:  x=bw+w, y=bw, w=bw, h=h
	 */

	if (!view->border[BORDER_TOP]) {
		/* First time — create border rects as children of scene_tree */
		for (int i = 0; i < BORDER_COUNT; i++) {
			view->border[i] = wlr_scene_rect_create(
				view->scene_tree, 1, 1, color);
		}
	}

	/* Top */
	wlr_scene_node_set_position(&view->border[BORDER_TOP]->node, 0, 0);
	wlr_scene_rect_set_size(view->border[BORDER_TOP], w + 2 * bw, bw);
	wlr_scene_rect_set_color(view->border[BORDER_TOP], color);
	wlr_scene_node_set_enabled(&view->border[BORDER_TOP]->node, true);

	/* Bottom */
	wlr_scene_node_set_position(&view->border[BORDER_BOTTOM]->node,
		0, bw + h);
	wlr_scene_rect_set_size(view->border[BORDER_BOTTOM], w + 2 * bw, bw);
	wlr_scene_rect_set_color(view->border[BORDER_BOTTOM], color);
	wlr_scene_node_set_enabled(&view->border[BORDER_BOTTOM]->node, true);

	/* Left */
	wlr_scene_node_set_position(&view->border[BORDER_LEFT]->node, 0, bw);
	wlr_scene_rect_set_size(view->border[BORDER_LEFT], bw, h);
	wlr_scene_rect_set_color(view->border[BORDER_LEFT], color);
	wlr_scene_node_set_enabled(&view->border[BORDER_LEFT]->node, true);

	/* Right */
	wlr_scene_node_set_position(&view->border[BORDER_RIGHT]->node,
		bw + w, bw);
	wlr_scene_rect_set_size(view->border[BORDER_RIGHT], bw, h);
	wlr_scene_rect_set_color(view->border[BORDER_RIGHT], color);
	wlr_scene_node_set_enabled(&view->border[BORDER_RIGHT]->node, true);
}

/* ── Focus ──────────────────────────────────────────────────────────── */

void view_focus(struct ew_server *server, struct ew_view *view) {
	input_focus_view(server, view);
}

/* ── App rule matching ──────────────────────────────────────────────── */

void view_apply_app_rules(struct ew_view *view) {
	struct ew_server *server = view->server;
	const char *app_id = view->xdg_toplevel->app_id;
	if (!app_id) return;

	for (int i = 0; i < server->config.num_app_rules; i++) {
		struct ew_app_rule *rule = &server->config.app_rules[i];
		if (!rule->app_id) continue;

		/* Match against app_id.  In Wayland there's no instance/class
		 * split like X11's WM_CLASS.  We do substring match for
		 * flexibility. */
		if (strcasecmp(app_id, rule->app_id) != 0 &&
		    !strstr(app_id, rule->app_id))
			continue;

		wlr_log(WLR_INFO, "app rule match: %s → rule %d", app_id, i);

		if (rule->has_geometry && !rule->ignore_position) {
			view_set_position(view, rule->gx, rule->gy);
			if (rule->gw > 0 && rule->gh > 0) {
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					rule->gw, rule->gh);
			}
		}
		if (rule->vdesk >= 0) {
			view->vdesk = rule->vdesk;
		}
		if (rule->fixed) {
			view->fixed = true;
		}
		if (rule->is_dock) {
			view->is_dock = true;
		}
		break; /* First match wins */
	}
}

/* ── XDG toplevel event handlers ────────────────────────────────────── */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct ew_view *view = wl_container_of(listener, view, map);
	(void)data;
	view->mapped = true;

	/* Apply app rules */
	view_apply_app_rules(view);

	/* Update borders */
	view_update_borders(view);

	/* Show/hide based on vdesk */
	if (!view->fixed && view->vdesk != view->server->current_vdesk) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	}

	/* Focus new window */
	view_focus(view->server, view);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct ew_view *view = wl_container_of(listener, view, unmap);
	(void)data;
	view->mapped = false;

	if (view->server->focused_view == view) {
		view->server->focused_view = NULL;
		/* Find another view to focus */
		struct ew_view *v;
		wl_list_for_each(v, &view->server->views, link) {
			if (v != view && v->mapped && !v->is_dock &&
			    (v->fixed || v->vdesk == view->server->current_vdesk)) {
				view_focus(view->server, v);
				break;
			}
		}
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct ew_view *view = wl_container_of(listener, view, commit);
	(void)data;
	if (view->xdg_toplevel->base->initial_commit) {
		/* First commit — configure without constraints */
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
		return;
	}

	if (view->mapped) {
		view_update_borders(view);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct ew_view *view = wl_container_of(listener, view, destroy);
	(void)data;

	/* SECURITY: clean up stale references — no use-after-free */
	if (view->server->focused_view == view)
		view->server->focused_view = NULL;
	if (view->server->grabbed_view == view) {
		view->server->grabbed_view = NULL;
		view->server->cursor_mode = EW_CURSOR_PASSTHROUGH;
	}

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	wl_list_remove(&view->link);

	free(view);
}

static void xdg_toplevel_request_move(struct wl_listener *listener,
                                      void *data) {
	(void)data;
	/* Clients can request interactive move (e.g. CSD drag).
	 * evilwm doesn't have CSD, but we respect the protocol. */
	/* Not implemented — evilwm has no CSD, all moves are compositor-driven */
}

static void xdg_toplevel_request_resize(struct wl_listener *listener,
                                        void *data) {
	(void)data;
	(void)listener;
	/* Same as move — compositor-driven only */
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
                                          void *data) {
	struct ew_view *view = wl_container_of(listener, view, request_maximize);
	(void)data;
	/* evilwm uses its own maximize toggle.  We respond to the protocol
	 * request by toggling both axes. */
	if (view->xdg_toplevel)
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
                                            void *data) {
	struct ew_view *view = wl_container_of(listener, view, request_fullscreen);
	(void)data;
	/* evilwm treats fullscreen as maximize-both.  Schedule configure. */
	if (view->xdg_toplevel)
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void xdg_toplevel_set_title_handler(struct wl_listener *listener,
                                           void *data) {
	(void)listener;
	(void)data;
}

static void xdg_toplevel_set_app_id_handler(struct wl_listener *listener,
                                            void *data) {
	(void)listener;
	(void)data;
}

/* ── New XDG toplevel ───────────────────────────────────────────────── */

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct ew_view *view = calloc(1, sizeof(*view));
	if (!view) {
		wlr_log(WLR_ERROR, "failed to allocate view");
		return;
	}

	view->server = server;
	view->xdg_toplevel = toplevel;
	view->vdesk = server->current_vdesk;
	view->fixed = false;
	view->is_dock = false;

	/* Create scene tree under the views layer */
	view->scene_tree = wlr_scene_xdg_surface_create(
		server->layers[EW_LAYER_VIEWS], toplevel->base);
	view->scene_tree->node.data = view;

	/* Position the surface within the scene tree (offset by border width) */
	/* The xdg surface node is the first child of scene_tree,
	 * we need to offset it by border_width so borders can frame it */
	int bw = server->config.border_width;
	/* wlr_scene_xdg_surface_create positions the surface at (0,0) within
	 * the tree.  We need the surface at (bw, bw) so borders can surround
	 * it.  We achieve this by setting the tree position to account for
	 * the border, and storing the view position as the inner position. */

	/* Default position: top-left of first output */
	view->x = bw;
	view->y = bw;

	/* Hook events */
	view->map.notify = xdg_toplevel_map;
	wl_signal_add(&toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&toplevel->events.destroy, &view->destroy);

	view->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize,
		&view->request_maximize);
	view->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen,
		&view->request_fullscreen);

	view->set_title.notify = xdg_toplevel_set_title_handler;
	wl_signal_add(&toplevel->events.set_title, &view->set_title);
	view->set_app_id.notify = xdg_toplevel_set_app_id_handler;
	wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);

	wl_list_insert(&server->views, &view->link);
}

/* ── New XDG popup ──────────────────────────────────────────────────── */

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_popup *popup = data;

	/* Create scene node for popup — parent is the popup's parent surface
	 * scene tree */
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent) return;

	struct wlr_scene_tree *parent_tree = parent->data;
	if (!parent_tree) return;

	popup->base->data = wlr_scene_xdg_surface_create(parent_tree,
		popup->base);
}

/* ── Init — called from main to set up xdg shell listener ──────────── */

void window_init(struct ew_server *server) {
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 6);

	server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);
	server->new_xdg_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup,
		&server->new_xdg_popup);
}
