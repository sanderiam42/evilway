/* func.c — bindable window manager functions
 *
 * Each function matches evilwm 1.5 behavior exactly.
 * The dispatch table maps enum ew_func_id to function pointers.
 * Flags use the FL_* system from evilwm's func.h.
 */

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "evilway.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static struct ew_view *focused(struct ew_server *s) {
	return s->focused_view;
}

/* Get usable area for the output containing a view */
static void get_output_box(struct ew_server *server, struct ew_view *view,
                           struct wlr_box *box) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, view->x, view->y);
	if (!output) {
		/* Fallback: first output */
		struct ew_output *eo;
		wl_list_for_each(eo, &server->outputs, link) {
			output = eo->wlr_output;
			break;
		}
	}
	if (!output) {
		box->x = box->y = 0;
		box->width = 1920;
		box->height = 1080;
		return;
	}

	if (server->config.wholescreen) {
		wlr_output_layout_get_box(server->output_layout, NULL, box);
	} else {
		wlr_output_layout_get_box(server->output_layout, output, box);
	}
}

static void get_view_dims(struct ew_view *view, int *w, int *h) {
	if (view->xdg_toplevel && view->xdg_toplevel->base->surface) {
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
		*w = geo.width;
		*h = geo.height;
	} else {
		*w = *h = 1;
	}
}

/* ── func_spawn ─────────────────────────────────────────────────────── */

void func_spawn(struct ew_server *server, unsigned flags) {
	(void)flags;
	const char *term = server->config.term;
	if (!term || !*term) term = "foot";

	pid_t pid = fork();
	if (pid == 0) {
		/* Child */
		setsid();
		execlp(term, term, NULL);
		_exit(127);
	} else if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed for %s", term);
	}
}

/* ── func_delete ────────────────────────────────────────────────────── */

void func_delete(struct ew_server *server, unsigned flags) {
	(void)flags;
	struct ew_view *view = focused(server);
	if (!view) return;
	/* Co-operative close via xdg_toplevel close event */
	wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

/* ── func_kill ──────────────────────────────────────────────────────── */

void func_kill(struct ew_server *server, unsigned flags) {
	(void)flags;
	struct ew_view *view = focused(server);
	if (!view) return;
	/* Force kill — send SIGKILL to the client process */
	struct wl_client *client = wl_resource_get_client(
		view->xdg_toplevel->base->surface->resource);
	if (client) {
		pid_t pid;
		wl_client_get_credentials(client, &pid, NULL, NULL);
		if (pid > 0)
			kill(pid, SIGKILL);
	}
}

/* ── func_lower ─────────────────────────────────────────────────────── */

void func_lower(struct ew_server *server, unsigned flags) {
	(void)flags;
	struct ew_view *view = focused(server);
	if (!view || !view->scene_tree) return;
	wlr_scene_node_lower_to_bottom(&view->scene_tree->node);
	/* Focus follows mouse — focus whatever is under cursor now */
	input_process_cursor_motion(server, 0);
}

/* ── func_raise ─────────────────────────────────────────────────────── */

void func_raise(struct ew_server *server, unsigned flags) {
	(void)flags;
	struct ew_view *view = focused(server);
	if (!view || !view->scene_tree) return;
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
}

/* ── func_next ──────────────────────────────────────────────────────── */

void func_next(struct ew_server *server, unsigned flags) {
	(void)flags;
	/* Cycle through mapped views on current vdesk */
	if (wl_list_empty(&server->views)) return;

	struct ew_view *current = focused(server);
	struct ew_view *next = NULL;

	/* Walk from current to find next visible view */
	struct wl_list *start = current ? &current->link : &server->views;
	struct wl_list *pos = start->next;

	while (pos != start) {
		if (pos == &server->views) {
			pos = pos->next;
			continue;
		}
		struct ew_view *v = wl_container_of(pos, v, link);
		if (v->mapped && !v->is_dock &&
		    (v->fixed || v->vdesk == server->current_vdesk)) {
			next = v;
			break;
		}
		pos = pos->next;
	}

	if (next && next != current) {
		view_focus(server, next);
		wlr_scene_node_raise_to_top(&next->scene_tree->node);
	}
}

/* ── func_move ──────────────────────────────────────────────────────── */

void func_move(struct ew_server *server, unsigned flags) {
	struct ew_view *view = focused(server);
	if (!view) return;

	int bw = server->config.border_width;
	int step = server->config.move_step;
	int snap = server->config.snap;
	struct wlr_box obox;
	get_output_box(server, view, &obox);

	int w, h;
	get_view_dims(view, &w, &h);

	int nx = view->x, ny = view->y;

	if (flags & FL_RELATIVE) {
		/* Move by step pixels in the given direction */
		if (flags & FL_LEFT)  nx -= step;
		if (flags & FL_RIGHT) nx += step;
		if (flags & FL_UP)    ny -= step;
		if (flags & FL_DOWN)  ny += step;
	} else {
		/* Move to edge/corner of monitor */
		if (flags & FL_LEFT)    nx = obox.x;
		if (flags & FL_RIGHT)   nx = obox.x + obox.width - w - 2 * bw;
		if (flags & FL_TOP)     ny = obox.y;
		if (flags & FL_BOTTOM)  ny = obox.y + obox.height - h - 2 * bw;
	}

	/* Snap to edges */
	if (snap > 0) {
		int right_edge = obox.x + obox.width - w - 2 * bw;
		int bottom_edge = obox.y + obox.height - h - 2 * bw;

		if (abs(nx - obox.x) < snap) nx = obox.x;
		if (abs(nx - right_edge) < snap) nx = right_edge;
		if (abs(ny - obox.y) < snap) ny = obox.y;
		if (abs(ny - bottom_edge) < snap) ny = bottom_edge;
	}

	view_set_position(view, nx, ny);
}

/* ── func_resize ────────────────────────────────────────────────────── */

void func_resize(struct ew_server *server, unsigned flags) {
	struct ew_view *view = focused(server);
	if (!view) return;

	int step = server->config.move_step;
	int bw = server->config.border_width;
	struct wlr_box obox;
	get_output_box(server, view, &obox);

	int w, h;
	get_view_dims(view, &w, &h);

	if (flags & FL_TOGGLE) {
		/* Toggle maximize along specified axes */
		bool toggle_h = (flags & FL_HORZ) != 0;
		bool toggle_v = (flags & FL_VERT) != 0;

		if (toggle_h) {
			if (view->maximized_h) {
				/* Restore horizontal */
				view_set_position(view, view->save_x, view->y);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					view->save_w, h);
				view->maximized_h = false;
			} else {
				/* Maximize horizontal */
				view->save_x = view->x;
				view->save_w = w;
				view_set_position(view, obox.x, view->y);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					obox.width - 2 * bw, h);
				view->maximized_h = true;
			}
		}
		if (toggle_v) {
			get_view_dims(view, &w, &h); /* re-read after h change */
			if (view->maximized_v) {
				/* Restore vertical */
				view_set_position(view, view->x, view->save_y);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					w, view->save_h);
				view->maximized_v = false;
			} else {
				/* Maximize vertical */
				view->save_y = view->y;
				view->save_h = h;
				view_set_position(view, view->x, obox.y);
				wlr_xdg_toplevel_set_size(view->xdg_toplevel,
					w, obox.height - 2 * bw);
				view->maximized_v = true;
			}
		}
	} else if (flags & FL_RELATIVE) {
		/* Resize by step in the given direction */
		int nw = w, nh = h;
		if (flags & FL_LEFT)   nw -= step;
		if (flags & FL_RIGHT)  nw += step;
		if (flags & FL_UP)     nh -= step;
		if (flags & FL_DOWN)   nh += step;
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, nw, nh);
	}
}

/* ── func_fix ───────────────────────────────────────────────────────── */

void func_fix(struct ew_server *server, unsigned flags) {
	struct ew_view *view = focused(server);
	if (!view) return;

	if (flags & FL_TOGGLE) {
		view->fixed = !view->fixed;
		view_update_borders(view);
		wlr_log(WLR_INFO, "view %s %s",
			view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "?",
			view->fixed ? "fixed" : "unfixed");
	}
}

/* ── func_dock ──────────────────────────────────────────────────────── */

void func_dock(struct ew_server *server, unsigned flags) {
	if (!(flags & FL_TOGGLE)) return;

	server->docks_visible = !server->docks_visible;

	struct ew_view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->is_dock && v->scene_tree) {
			wlr_scene_node_set_enabled(&v->scene_tree->node,
				server->docks_visible);
		}
	}
}

/* ── func_info ──────────────────────────────────────────────────────── */

void func_info(struct ew_server *server, unsigned flags) {
	(void)flags;
	struct ew_view *view = focused(server);
	if (!view) return;

	int w, h;
	get_view_dims(view, &w, &h);

	/* evilwm shows info as long as key is held.  For now, log to stderr.
	 * FUTURE: render an overlay via wlr_scene_buffer. */
	const char *app = view->xdg_toplevel->app_id;
	const char *title = view->xdg_toplevel->title;
	wlr_log(WLR_INFO, "INFO: app_id=%s title=\"%s\" pos=%d,%d size=%dx%d "
		"vdesk=%d fixed=%s",
		app ? app : "(null)",
		title ? title : "(null)",
		view->x, view->y, w, h,
		view->vdesk,
		view->fixed ? "yes" : "no");
}

/* ── func_vdesk ─────────────────────────────────────────────────────── */

void func_vdesk(struct ew_server *server, unsigned flags) {
	if (flags & FL_TOGGLE) {
		vdesk_switch_toggle(server);
	} else if (flags & FL_RELATIVE) {
		vdesk_switch_relative(server, flags);
	} else {
		int target = flags & FL_VALUEMASK;
		vdesk_switch(server, target);
	}
}

/* ── Dispatch table ─────────────────────────────────────────────────── */

void func_dispatch(struct ew_server *server, enum ew_func_id func,
                   unsigned flags) {
	switch (func) {
	case EW_FUNC_SPAWN:   func_spawn(server, flags);   break;
	case EW_FUNC_DELETE:  func_delete(server, flags);  break;
	case EW_FUNC_KILL:    func_kill(server, flags);    break;
	case EW_FUNC_LOWER:   func_lower(server, flags);   break;
	case EW_FUNC_RAISE:   func_raise(server, flags);   break;
	case EW_FUNC_NEXT:    func_next(server, flags);    break;
	case EW_FUNC_MOVE:    func_move(server, flags);    break;
	case EW_FUNC_RESIZE:  func_resize(server, flags);  break;
	case EW_FUNC_FIX:     func_fix(server, flags);     break;
	case EW_FUNC_DOCK:    func_dock(server, flags);    break;
	case EW_FUNC_INFO:    func_info(server, flags);    break;
	case EW_FUNC_VDESK:   func_vdesk(server, flags);   break;
	case EW_FUNC_QUIT:
		wl_display_terminate(server->wl_display);
		break;
	case EW_FUNC_NONE:
		break;
	}
}
