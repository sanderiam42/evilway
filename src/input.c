/* input.c — keyboard and pointer input handling
 *
 * SECURITY: compositor keybindings are consumed before dispatch to clients.
 * A client can never intercept Super+Shift+Q or the lock screen invocation.
 *
 * Key repeat for compositor binds: when a bound key is held, the action
 * repeats at EW_REPEAT_RATE_MS after an initial EW_REPEAT_DELAY_MS pause.
 * This matches the user requirement that holding Super+L (etc.) continuously
 * moves the window.
 */

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "evilway.h"

/* ── Key repeat timer ───────────────────────────────────────────────── */

static int key_repeat_timer_cb(void *data) {
	struct ew_server *server = data;
	struct ew_key_repeat *kr = &server->key_repeat;
	if (!kr->active || !kr->bind) return 0;

	func_dispatch(server, kr->bind->func, kr->bind->flags);

	/* Re-arm at the repeat rate */
	wl_event_source_timer_update(kr->timer, EW_REPEAT_RATE_MS);
	return 0;
}

static void key_repeat_start(struct ew_server *server, struct ew_bind *bind) {
	struct ew_key_repeat *kr = &server->key_repeat;
	kr->bind = bind;
	kr->active = true;
	wl_event_source_timer_update(kr->timer, EW_REPEAT_DELAY_MS);
}

static void key_repeat_stop(struct ew_server *server) {
	struct ew_key_repeat *kr = &server->key_repeat;
	kr->active = false;
	kr->bind = NULL;
	wl_event_source_timer_update(kr->timer, 0);
}

/* ── Should this bind repeat when held? ─────────────────────────────── */

static bool bind_is_repeatable(struct ew_bind *bind) {
	switch (bind->func) {
	case EW_FUNC_MOVE:
		return (bind->flags & FL_RELATIVE) != 0;
	case EW_FUNC_RESIZE:
		return (bind->flags & FL_RELATIVE) != 0;
	case EW_FUNC_VDESK:
		return (bind->flags & FL_RELATIVE) != 0;
	default:
		return false;
	}
}

/* ── Keyboard handling ──────────────────────────────────────────────── */

/* Check if key event matches a compositor binding and execute it.
 * Returns true if the key was consumed (not forwarded to client). */
static bool handle_compositor_keybinding(struct ew_server *server,
                                         xkb_keysym_t sym, uint32_t mods,
                                         bool pressed) {
	struct ew_config *cfg = &server->config;

	for (int i = 0; i < cfg->num_binds; i++) {
		struct ew_bind *b = &cfg->binds[i];
		if (b->type != EW_BIND_KEY) continue;
		if (b->keysym != sym) continue;
		if (b->modifiers != mods) continue;

		if (pressed) {
			/* Execute the function */
			func_dispatch(server, b->func, b->flags);

			/* Start key repeat if this is a repeatable action */
			if (bind_is_repeatable(b)) {
				key_repeat_start(server, b);
			}
		}
		return true; /* Consumed, even on release */
	}

	return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct ew_keyboard *kb = wl_container_of(listener, kb, key);
	struct ew_server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;

	/* SECURITY: session lock check — if locked, drop all input except
	 * what the lock surface should receive */
	if (server->active_lock) {
		/* Forward to lock surface only */
		wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
		return;
	}

	uint32_t keycode = event->keycode + 8;  /* evdev to xkb offset */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
		kb->wlr_keyboard->xkb_state, keycode, &syms);

	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	bool pressed = (event->state == WL_KEYBOARD_KEY_STATE_PRESSED);

	/* On key release, stop any active key repeat */
	if (!pressed) {
		key_repeat_stop(server);
	}

	bool handled = false;
	for (int i = 0; i < nsyms; i++) {
		if (handle_compositor_keybinding(server, syms[i], mods, pressed)) {
			handled = true;
			break;
		}
	}

	if (!handled) {
		/* Forward to the focused client */
		wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct ew_keyboard *kb = wl_container_of(listener, kb, modifiers);
	(void)data;
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
		&kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct ew_keyboard *kb = wl_container_of(listener, kb, destroy);
	(void)data;
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

static void new_keyboard(struct ew_server *server,
                         struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);

	struct ew_keyboard *kb = calloc(1, sizeof(*kb));
	if (!kb) return;
	kb->server = server;
	kb->wlr_keyboard = wlr_kb;

	/* Keymap — default layout, user's environment determines layout */
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_kb, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);

	wlr_keyboard_set_repeat_info(wlr_kb, 25, 400);

	kb->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_kb->events.key, &kb->key);
	kb->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_kb);
	wl_list_insert(&server->keyboards, &kb->link);
}

/* ── Pointer handling ───────────────────────────────────────────────── */

/* Focus follows mouse — evilwm's core interaction model */
void input_process_cursor_motion(struct ew_server *server, uint32_t time) {
	/* If we're in a grab, handle move/resize */
	if (server->cursor_mode == EW_CURSOR_MOVE) {
		struct ew_view *view = server->grabbed_view;
		if (view) {
			int nx = server->cursor->x - server->grab_x;
			int ny = server->cursor->y - server->grab_y;

			/* Snap */
			int bw = server->config.border_width;
			int snap = server->config.snap;
			if (snap > 0) {
				struct wlr_box obox;
				int w, h;
				struct wlr_box geo;
				wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
				w = geo.width; h = geo.height;
				struct wlr_output *out = wlr_output_layout_output_at(
					server->output_layout, server->cursor->x,
					server->cursor->y);
				if (out) {
					wlr_output_layout_get_box(server->output_layout,
						out, &obox);
				} else {
					wlr_output_layout_get_box(server->output_layout,
						NULL, &obox);
				}

				int re = obox.x + obox.width - w - 2 * bw;
				int be = obox.y + obox.height - h - 2 * bw;
				if (abs(nx - obox.x) < snap) nx = obox.x;
				if (abs(nx - re) < snap) nx = re;
				if (abs(ny - obox.y) < snap) ny = obox.y;
				if (abs(ny - be) < snap) ny = be;
			}

			view_set_position(view, nx, ny);
		}
		return;
	}

	if (server->cursor_mode == EW_CURSOR_RESIZE) {
		struct ew_view *view = server->grabbed_view;
		if (view) {
			double dx = server->cursor->x - server->grab_x;
			double dy = server->cursor->y - server->grab_y;
			int nw = server->grab_geobox.width;
			int nh = server->grab_geobox.height;
			int nx = view->x;
			int ny = view->y;

			if (server->resize_edges & WLR_EDGE_RIGHT)
				nw = server->grab_geobox.width + (int)dx;
			else if (server->resize_edges & WLR_EDGE_LEFT) {
				nw = server->grab_geobox.width - (int)dx;
				nx = server->grab_geobox.x + (int)dx;
			}
			if (server->resize_edges & WLR_EDGE_BOTTOM)
				nh = server->grab_geobox.height + (int)dy;
			else if (server->resize_edges & WLR_EDGE_TOP) {
				nh = server->grab_geobox.height - (int)dy;
				ny = server->grab_geobox.y + (int)dy;
			}

			if (nw < 1) nw = 1;
			if (nh < 1) nh = 1;

			view_set_position(view, nx, ny);
			wlr_xdg_toplevel_set_size(view->xdg_toplevel, nw, nh);
		}
		return;
	}

	/* Passthrough mode — focus follows mouse */
	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct ew_view *view = view_at(server, server->cursor->x,
		server->cursor->y, &surface, &sx, &sy);

	if (!surface) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
		wlr_seat_pointer_clear_focus(server->seat);
		return;
	}

	/* Focus follows mouse: focus the view under cursor */
	if (view && view != server->focused_view) {
		view_focus(server, view);
	}

	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
}

/* Begin interactive move/resize via mouse */
static void begin_interactive(struct ew_server *server, struct ew_view *view,
                              enum ew_cursor_mode mode, uint32_t edges) {
	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == EW_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
		server->grab_x = server->cursor->x;
		server->grab_y = server->cursor->y;
		server->grab_geobox.x = view->x;
		server->grab_geobox.y = view->y;
		server->grab_geobox.width = geo.width;
		server->grab_geobox.height = geo.height;
		server->resize_edges = edges;
	}
}

static void cursor_handle_motion(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	input_process_cursor_motion(server, event->time_msec);
}

static void cursor_handle_motion_absolute(struct wl_listener *listener,
                                          void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	input_process_cursor_motion(server, event->time_msec);
}

static void cursor_handle_button(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* End any grab */
		if (server->cursor_mode != EW_CURSOR_PASSTHROUGH) {
			server->cursor_mode = EW_CURSOR_PASSTHROUGH;
			server->grabbed_view = NULL;
			input_process_cursor_motion(server, event->time_msec);
		}
		wlr_seat_pointer_notify_button(server->seat, event->time_msec,
			event->button, event->state);
		return;
	}

	/* Button pressed */
	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct ew_view *view = view_at(server, server->cursor->x,
		server->cursor->y, &surface, &sx, &sy);

	/* Check modifier — evilwm uses mask2 for mouse actions anywhere
	 * in window.  Without modifier, button on border edge triggers. */
	uint32_t mods = 0;
	struct ew_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		mods |= wlr_keyboard_get_modifiers(kb->wlr_keyboard);
		break; /* just need one keyboard's modifier state */
	}

	bool has_mask2 = (mods & server->config.mask2) == server->config.mask2;

	if (view && has_mask2) {
		/* Check button binds */
		struct ew_config *cfg = &server->config;
		for (int i = 0; i < cfg->num_binds; i++) {
			struct ew_bind *b = &cfg->binds[i];
			if (b->type != EW_BIND_BUTTON) continue;
			if (b->button != event->button) continue;

			/* Focus and raise the view first */
			view_focus(server, view);
			wlr_scene_node_raise_to_top(&view->scene_tree->node);

			if (b->func == EW_FUNC_MOVE) {
				begin_interactive(server, view, EW_CURSOR_MOVE, 0);
			} else if (b->func == EW_FUNC_RESIZE) {
				/* Resize from bottom-right by default (evilwm behavior) */
				begin_interactive(server, view, EW_CURSOR_RESIZE,
					WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
			} else if (b->func == EW_FUNC_LOWER) {
				func_lower(server, 0);
			} else {
				func_dispatch(server, b->func, b->flags);
			}
			return;
		}
	}

	if (view) {
		view_focus(server, view);
	}

	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);
}

static void cursor_handle_axis(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void cursor_handle_frame(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		cursor_frame);
	(void)data;
	wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;
	if (focused == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener,
                                       void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ── New input device ───────────────────────────────────────────────── */

static void new_input(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}

	/* Announce capabilities */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server->seat, caps);
}

/* ── Focus a view — set keyboard focus, update borders ──────────────── */

void input_focus_view(struct ew_server *server, struct ew_view *view) {
	struct ew_view *prev = server->focused_view;

	if (prev == view) return;

	if (prev && prev->mapped) {
		/* Unfocus: set inactive border color */
		for (int i = 0; i < BORDER_COUNT; i++) {
			if (prev->border[i]) {
				wlr_scene_rect_set_color(prev->border[i],
					server->config.bg);
			}
		}
		if (prev->xdg_toplevel)
			wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
	}

	server->focused_view = view;

	if (!view) {
		wlr_seat_keyboard_clear_focus(server->seat);
		return;
	}

	/* Set active/fixed border color */
	float *color = view->fixed ? server->config.fc : server->config.fg;
	for (int i = 0; i < BORDER_COUNT; i++) {
		if (view->border[i])
			wlr_scene_rect_set_color(view->border[i], color);
	}

	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(server->seat,
			view->xdg_toplevel->base->surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
}

/* ── Init ───────────────────────────────────────────────────────────── */

void input_init(struct ew_server *server) {
	/* Cursor */
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server->cursor_motion.notify = cursor_handle_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = cursor_handle_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
		&server->cursor_motion_absolute);
	server->cursor_button.notify = cursor_handle_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = cursor_handle_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = cursor_handle_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	/* Seat */
	wl_list_init(&server->keyboards);
	server->seat = wlr_seat_create(server->wl_display, "seat0");

	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);

	server->new_input.notify = new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	/* Key repeat timer */
	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	server->key_repeat.timer = wl_event_loop_add_timer(loop,
		key_repeat_timer_cb, server);
	server->key_repeat.active = false;
	server->key_repeat.bind = NULL;
}
