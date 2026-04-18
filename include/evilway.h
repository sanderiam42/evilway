/* evilWay — Wayland compositor implementing evilwm behavior
 *
 * Not a port.  evilwm is X11-only.  evilWay is a new compositor written
 * in C on wlroots, using evilwm 1.5 as the behavior specification.
 *
 * evilwm by Ciaran Anscomb: https://www.6809.org.uk/evilwm/
 * wlroots: https://gitlab.freedesktop.org/wlroots/wlroots
 * Architectural reference: dwl (https://codeberg.org/dwl/dwl)
 */

#ifndef EVILWAY_H
#define EVILWAY_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* ── Limits ─────────────────────────────────────────────────────────── */

#define EW_MAX_BINDS        128
#define EW_MAX_APP_RULES    64

/* ── Defaults ───────────────────────────────────────────────────────── */

/* DECISION: mask1 defaults to Super (Mod4), not Ctrl+Alt like evilwm.
 * Rationale: on Apple hardware, Command maps to Super/Mod4.  Using
 * Ctrl+Alt would collide with terminal emulator binds (Ctrl+Alt+T, etc).
 * mask2 also defaults to Super for consistency.  Users can override
 * via .evilwayrc just like evilwm's .evilwmrc. */

#define EW_DEFAULT_BW         1
#define EW_DEFAULT_SNAP       0       /* 0 = snap disabled */
#define EW_DEFAULT_VDESKS_C   8
#define EW_DEFAULT_VDESKS_R   1
#define EW_DEFAULT_MOVE_STEP  16
#define EW_DEFAULT_TERM       "foot"

/* Key repeat for compositor bindings (ms) */
#define EW_REPEAT_DELAY_MS    400
#define EW_REPEAT_RATE_MS     30

/* ── Function flags (mirrors evilwm func.h architecture) ────────────
 * Lower 8 bits: numeric value (vdesk number, etc.)
 * Upper bits: behavioral flags.
 * Packing value+flags into one unsigned is evilwm's design; we keep it. */

#define FL_VALUEMASK  (0xff)
#define FL_UP         (1 << 8)
#define FL_DOWN       (1 << 9)
#define FL_LEFT       (1 << 10)
#define FL_RIGHT      (1 << 11)
#define FL_TOP        (1 << 12)
#define FL_BOTTOM     (1 << 13)
#define FL_RELATIVE   (1 << 14)
#define FL_TOGGLE     (1 << 18)

/* Convenience combos */
#define FL_TOPLEFT      (FL_TOP | FL_LEFT)
#define FL_TOPRIGHT     (FL_TOP | FL_RIGHT)
#define FL_BOTTOMLEFT   (FL_BOTTOM | FL_LEFT)
#define FL_BOTTOMRIGHT   (FL_BOTTOM | FL_RIGHT)
#define FL_VERT         (FL_TOP | FL_BOTTOM)
#define FL_HORZ         (FL_LEFT | FL_RIGHT)

/* ── Enumerations ───────────────────────────────────────────────────── */

enum ew_cursor_mode {
	EW_CURSOR_PASSTHROUGH,
	EW_CURSOR_MOVE,
	EW_CURSOR_RESIZE,
};

/* Bindable function IDs — matches evilwm's function list exactly */
enum ew_func_id {
	EW_FUNC_NONE = 0,
	EW_FUNC_SPAWN,
	EW_FUNC_DELETE,
	EW_FUNC_KILL,
	EW_FUNC_LOWER,
	EW_FUNC_RAISE,
	EW_FUNC_NEXT,
	EW_FUNC_MOVE,
	EW_FUNC_RESIZE,
	EW_FUNC_FIX,
	EW_FUNC_DOCK,
	EW_FUNC_INFO,
	EW_FUNC_VDESK,
	/* compositor-only, not in evilwm */
	EW_FUNC_QUIT,
};

enum ew_bind_type {
	EW_BIND_KEY,
	EW_BIND_BUTTON,
};

/* Border rect indices */
enum {
	BORDER_TOP = 0,
	BORDER_BOTTOM,
	BORDER_LEFT,
	BORDER_RIGHT,
	BORDER_COUNT,
};

/* Scene-tree layer ordering */
enum ew_layer {
	EW_LAYER_BACKGROUND = 0,
	EW_LAYER_BOTTOM,
	EW_LAYER_VIEWS,      /* normal windows live here */
	EW_LAYER_TOP,
	EW_LAYER_OVERLAY,
	EW_LAYER_LOCK,        /* session-lock surface */
	EW_LAYER_COUNT,
};

/* ── Structures ─────────────────────────────────────────────────────── */

struct ew_server;

/* ── Bind ───────────────────────────────────────────────────────────── */

struct ew_bind {
	enum ew_bind_type type;
	xkb_keysym_t      keysym;    /* for key binds */
	uint32_t           button;    /* for button binds (BTN_LEFT etc) */
	uint32_t           modifiers; /* WLR_MODIFIER_* bitmask */
	enum ew_func_id    func;
	unsigned           flags;     /* FL_* | value in lower 8 bits */
};

/* ── Application matching rule ──────────────────────────────────────── */

struct ew_app_rule {
	char    *app_id;             /* Wayland app_id (was WM_CLASS in X11) */
	bool     has_geometry;
	int      gx, gy, gw, gh;    /* parsed geometry */
	int      vdesk;              /* -1 = no preference */
	bool     fixed;
	bool     is_dock;
	bool     ignore_position;
	bool     ignore_border;
};

/* ── Configuration ──────────────────────────────────────────────────── */

struct ew_config {
	char    *term;

	/* Border colors (RGBA as float[4] for wlr_scene_rect) */
	float    fg[4];              /* active window */
	float    fc[4];              /* fixed window */
	float    bg[4];              /* inactive window */

	int      border_width;
	int      snap;
	int      move_step;
	bool     nosoliddrag;
	bool     wholescreen;

	int      vdesks_cols;
	int      vdesks_rows;

	/* Modifier masks — stored as WLR_MODIFIER_* bitmask */
	uint32_t mask1;
	uint32_t mask2;
	uint32_t altmask;

	/* Binds */
	struct ew_bind binds[EW_MAX_BINDS];
	int            num_binds;

	/* App rules */
	struct ew_app_rule app_rules[EW_MAX_APP_RULES];
	int                num_app_rules;
};

/* ── View (client window) ───────────────────────────────────────────── */

struct ew_view {
	struct wl_list              link;       /* ew_server.views */
	struct ew_server           *server;
	struct wlr_xdg_toplevel    *xdg_toplevel;
	struct wlr_scene_tree      *scene_tree;

	/* Border rects — four edges around the surface */
	struct wlr_scene_rect      *border[BORDER_COUNT];

	/* State */
	int      x, y;              /* compositor-space position */
	int      vdesk;
	bool     fixed;             /* visible on all vdesks */
	bool     is_dock;
	bool     mapped;
	bool     urgent;

	/* Maximize toggle state (evilwm saves geometry before maximize) */
	bool     maximized_h;
	bool     maximized_v;
	int      save_x, save_y, save_w, save_h;

	/* Listeners */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
};

/* ── Layer surface ──────────────────────────────────────────────────── */

struct ew_layer_surface {
	struct wl_list                  link;
	struct wlr_layer_surface_v1    *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct ew_server               *server;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
};

/* ── Output ─────────────────────────────────────────────────────────── */

struct ew_output {
	struct wl_list          link;
	struct ew_server       *server;
	struct wlr_output      *wlr_output;
	struct wlr_scene_output *scene_output;

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

/* ── Keyboard ───────────────────────────────────────────────────────── */

struct ew_keyboard {
	struct wl_list          link;
	struct ew_server       *server;
	struct wlr_keyboard    *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* ── Session lock ───────────────────────────────────────────────────── */

struct ew_session_lock {
	struct wlr_session_lock_v1 *lock;
	struct wlr_scene_tree      *scene_tree;
	struct ew_server           *server;

	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
};

/* ── Key repeat for compositor bindings ─────────────────────────────── */

struct ew_key_repeat {
	struct wl_event_source *timer;
	struct ew_bind         *bind;
	bool                    active;
};

/* ── Server (top-level state) ───────────────────────────────────────── */

struct ew_server {
	struct wl_display          *wl_display;
	struct wlr_backend         *backend;
	struct wlr_renderer        *renderer;
	struct wlr_allocator       *allocator;

	/* Scene graph */
	struct wlr_scene                *scene;
	struct wlr_scene_output_layout  *scene_layout;
	struct wlr_scene_tree           *layers[EW_LAYER_COUNT];

	/* Shells */
	struct wlr_xdg_shell          *xdg_shell;
	struct wlr_layer_shell_v1     *layer_shell;
	struct wlr_session_lock_manager_v1 *session_lock_mgr;

	/* Output */
	struct wlr_output_layout *output_layout;
	struct wl_list            outputs;

	/* Input */
	struct wlr_seat    *seat;
	struct wlr_cursor  *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_list      keyboards;

	/* Grab state — move/resize in progress */
	enum ew_cursor_mode cursor_mode;
	struct ew_view     *grabbed_view;
	double              grab_x, grab_y;
	struct wlr_box      grab_geobox;
	uint32_t            resize_edges;

	/* Window list */
	struct wl_list      views;
	struct ew_view     *focused_view;

	/* Virtual desktops */
	int current_vdesk;
	int prev_vdesk;

	/* Dock visibility */
	bool docks_visible;

	/* Session lock */
	struct ew_session_lock *active_lock;

	/* Key repeat for compositor binds */
	struct ew_key_repeat key_repeat;

	/* Configuration */
	struct ew_config config;

	/* Listeners — shell events */
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_layer_surface;
	struct wl_listener new_session_lock;

	/* Listeners — output */
	struct wl_listener new_output;

	/* Listeners — input */
	struct wl_listener new_input;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
};

/* ── Function prototypes ────────────────────────────────────────────── */

/* config.c */
void config_init(struct ew_config *cfg);
void config_load(struct ew_config *cfg, const char *path);
void config_destroy(struct ew_config *cfg);

/* input.c */
void input_init(struct ew_server *server);
void input_focus_view(struct ew_server *server, struct ew_view *view);
void input_process_cursor_motion(struct ew_server *server, uint32_t time);

/* output.c */
void output_init(struct ew_server *server);

/* window.c */
struct ew_view *view_at(struct ew_server *server, double lx, double ly,
                        struct wlr_surface **surface, double *sx, double *sy);
void view_focus(struct ew_server *server, struct ew_view *view);
void view_set_position(struct ew_view *view, int x, int y);
void view_update_borders(struct ew_view *view);
void view_apply_app_rules(struct ew_view *view);
void window_init(struct ew_server *server);

/* vdesk.c */
void vdesk_switch(struct ew_server *server, int vdesk);
void vdesk_switch_relative(struct ew_server *server, unsigned flags);
void vdesk_switch_toggle(struct ew_server *server);
void vdesk_update_visibility(struct ew_server *server);

/* func.c — bindable functions (evilwm-compatible dispatch) */
void func_spawn(struct ew_server *server, unsigned flags);
void func_delete(struct ew_server *server, unsigned flags);
void func_kill(struct ew_server *server, unsigned flags);
void func_lower(struct ew_server *server, unsigned flags);
void func_raise(struct ew_server *server, unsigned flags);
void func_next(struct ew_server *server, unsigned flags);
void func_move(struct ew_server *server, unsigned flags);
void func_resize(struct ew_server *server, unsigned flags);
void func_fix(struct ew_server *server, unsigned flags);
void func_dock(struct ew_server *server, unsigned flags);
void func_info(struct ew_server *server, unsigned flags);
void func_vdesk(struct ew_server *server, unsigned flags);
void func_dispatch(struct ew_server *server, enum ew_func_id func,
                   unsigned flags);

/* layer.c */
void layer_init(struct ew_server *server);

/* lock.c */
void lock_init(struct ew_server *server);

#endif /* EVILWAY_H */
