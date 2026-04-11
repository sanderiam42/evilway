/* evilway.h — shared type definitions for the evilWay compositor
 *
 * Struct definitions and constants only. No implementation here.
 *
 * Target: wlroots-0.19 (Fedora 43: 0.19.2-1.fc43)
 */
#ifndef EVILWAY_H
#define EVILWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "window.h"

/*
 * Compositor modifier key.
 *
 * Super (Mod4) maps to the Command (⌘) key on Apple keyboards under
 * Linux/Asahi. This deliberately diverges from evilwm's Alt-based bindings:
 * Command is the natural "WM key" on this hardware and avoids conflicts
 * with terminal and application Alt shortcuts.
 *
 * Used exclusively for the hardcoded Super+Shift+Q emergency exit, which is
 * not user-configurable. All other bindings use the EwConfig bind system.
 */
#define MODIFIER WLR_MODIFIER_LOGO

/*
 * Movement and resize constants.
 *
 * MOVE_STEP matches evilwm's default 16-pixel keyboard increment for both
 * move and resize operations. MIN_WIN_CONTENT is the minimum CLIENT content
 * dimension (not including borders); outer minimum = MIN_WIN_CONTENT + 2*bw.
 */
#define MOVE_STEP        16   /* keyboard move/resize step in pixels */
#define MIN_WIN_CONTENT  32   /* minimum client content width or height */

/*
 * Border colors as packed RGBA uint32_t (0xRRGGBBAA).
 *
 * BORDER_COLOR_ACTIVE matches evilwm's classic gold (#DAA520).
 * BORDER_COLOR_INACTIVE is dark grey (#444444).
 *
 * These are compile-time constants, not config values. Color configuration
 * will be added in a later pass when the .evilwayrc color keys are defined.
 *
 * SECURITY: Both values are literals — no client-supplied input can influence
 * border rendering colors.
 */
#define BORDER_COLOR_ACTIVE   0xDAA520FFu  /* evilwm gold */
#define BORDER_COLOR_INACTIVE 0x444444FFu  /* dark grey */

/* =========================================================================
 * Runtime config — bind flags, function enum, bind and config structs.
 *
 * These are populated by src/config.c from ~/.evilwayrc on startup and on
 * SIGHUP. The compile-time "#define TERMINAL" is gone; the terminal is now
 * EwConfig.terminal, defaulting to "foot" when no config file is present.
 * ====================================================================== */

/*
 * Flags for bind actions. Combine with | to express compound behaviors:
 *   FLAG_RELATIVE|FLAG_LEFT  → move/resize incrementally leftward
 *   FLAG_TOGGLE|FLAG_VERTICAL|FLAG_HORIZONTAL → full maximize toggle
 *   FLAG_TOP|FLAG_LEFT       → snap window to top-left corner
 */
#define FLAG_RELATIVE    (1u << 0)
#define FLAG_TOGGLE      (1u << 1)
#define FLAG_TOP         (1u << 2)
#define FLAG_BOTTOM      (1u << 3)
#define FLAG_LEFT        (1u << 4)
#define FLAG_RIGHT       (1u << 5)
#define FLAG_UP          (1u << 6)
#define FLAG_DOWN        (1u << 7)
#define FLAG_HORIZONTAL  (1u << 8)
#define FLAG_VERTICAL    (1u << 9)

/* Window-management functions available to user-defined binds. */
typedef enum {
    FUNC_SPAWN,   /* spawn the configured terminal */
    FUNC_DELETE,  /* request window close (graceful) */
    FUNC_KILL,    /* force-kill window's client */
    FUNC_LOWER,   /* lower window in stacking order */
    FUNC_RAISE,   /* raise window to top of stack */
    FUNC_MOVE,    /* move window (flags: relative+dir, corner) */
    FUNC_RESIZE,  /* resize window (flags: relative+dir, toggle) */
    FUNC_FIX,     /* toggle window fixed (sticky across vdesks) */
    FUNC_VDESK,   /* switch virtual desktop (flags or vdesk_target) */
    FUNC_NEXT,    /* cycle focus to next window */
    FUNC_DOCK,    /* toggle dock visibility */
    FUNC_INFO,    /* show window info overlay */
} EwFunction;

/*
 * EwBind — a single keybinding or mouse button binding.
 *
 * is_mouse=false: keyboard bind. Match on modifiers (WLR_MODIFIER_* bits)
 *   and keysym (resolved by xkb_keysym_from_name() at config parse time).
 * is_mouse=true:  mouse button bind. Match on button (1–5, config numbering).
 *   The evdev↔config mapping is done in main.c at dispatch time.
 *
 * vdesk_target: for FUNC_VDESK with a bare integer target (e.g. "vdesk,0").
 *   Set to -1 when not applicable (all other functions, and vdesk with flags).
 */
typedef struct {
    bool         is_mouse;
    uint32_t     modifiers;     /* WLR_MODIFIER_* bitmask; 0 for mouse binds */
    xkb_keysym_t keysym;        /* XKB keysym; XKB_KEY_NoSymbol for mouse binds */
    uint32_t     button;        /* 1–5 (config numbering); 0 for keyboard binds */
    EwFunction   function;
    uint32_t     flags;         /* FLAG_* bitmask */
    int          vdesk_target;  /* desktop index for vdesk,N; -1 if unused */
} EwBind;

/*
 * EwConfig — full compositor runtime configuration.
 *
 * Lives inside struct Server on the stack. Populated by config_load() and
 * freed by config_free(). config_set_defaults() establishes fallback values
 * used when no config file is present.
 *
 * SECURITY: terminal[] is bounded to 255 chars + NUL. All string fields are
 * fixed-size arrays. No unbounded heap strings in this struct.
 */
typedef struct {
    int    border_width;     /* bw: window border width in pixels (default 1, range 1–20) */
    int    snap_distance;    /* snap: snap-to-edge distance in pixels (default 0=disabled, range 0–100) */
    char   terminal[256];    /* term: terminal to spawn on FUNC_SPAWN (default "foot") */
    int    num_vdesks;       /* numvdesks: number of virtual desktops (default 4, range 1–16) */
    EwBind *binds;           /* heap-allocated array of key/mouse bindings */
    size_t  num_binds;       /* number of valid entries in binds[] */
    size_t  binds_capacity;  /* allocated capacity of binds[] */
} EwConfig;

/* =========================================================================
 * Scene layer z-ordering.
 *
 * Layer trees are created as children of the root wlr_scene in this order.
 * Lower index = rendered first (bottom), higher index = rendered last (top).
 *
 * Matches dwl's layer model with tiling-specific layers removed (evilWay is
 * floating-only). Additional layers (LyrBottom, LyrOverlay, etc.) will be
 * inserted here as protocols are added in subsequent phases.
 *
 * SECURITY: LyrBlock must remain the topmost layer. Session lock surfaces
 * (ext-session-lock-v1, Phase 2) will be placed here, above all client content.
 * If any other layer is accidentally placed above LyrBlock, the lock screen
 * can be obscured and input may reach client windows while locked.
 * ====================================================================== */
enum {
    LyrBg,      /* solid background fill */
    LyrFloat,   /* all toplevel windows — evilWay is floating-only */
    LyrTop,     /* reserved: wlr-layer-shell top/overlay surfaces (Phase 2) */
    LyrBlock,   /* reserved: ext-session-lock-v1 lock surfaces (Phase 2) */
    LYR_COUNT
};

/*
 * EwCursorMode — tracks whether the compositor has an active mouse grab.
 *
 * CurNormal:  no grab; pointer events are forwarded to clients normally.
 * CurMove:    Super+button1 drag in progress; compositor is moving a window.
 * CurResize:  Super+button2 drag in progress; compositor is resizing a window.
 *
 * SECURITY: While cursor_mode != CurNormal, the compositor consumes ALL
 * pointer motion and button events without forwarding them to any client.
 * This is enforced in process_cursor_motion() and handle_cursor_button() in
 * main.c. A client that receives pointer events during a compositor grab is
 * a correctness bug and a potential security issue (it can observe cursor
 * position outside its own surface).
 */
typedef enum { CurNormal, CurMove, CurResize } EwCursorMode;

/*
 * Server — top-level compositor state.
 *
 * One instance, lives on the stack in main(). Passed by pointer to all
 * handlers via wl_container_of(). Do not heap-allocate this struct.
 */
struct Server {
    struct wl_display            *display;
    struct wlr_backend           *backend;
    struct wlr_renderer          *renderer;
    struct wlr_allocator         *allocator;

    /* Populated by wlr_backend_autocreate() on DRM/KMS (bare hardware).
     * NULL when running nested (Wayland/X11 backend). Used for VT switching.
     * wlr_session_change_vt() is a no-op when session is NULL. */
    struct wlr_session           *session;

    /* Scene graph: all rendering flows through here.
     * Never bypass wlr_scene — direct rendering breaks damage tracking,
     * screencopy, and session lock. */
    struct wlr_scene             *scene;
    struct wlr_scene_tree        *layers[LYR_COUNT];
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_output_layout     *output_layout;
    struct wl_list                outputs;    /* Output::link */

    struct wlr_xdg_shell         *xdg_shell;
    struct wl_list                toplevels;  /* Toplevel::link, front = focused */

    /* Keyboard list — one Keyboard per physical keyboard device.
     * TODO: migrate to wlr_keyboard_group so modifier state is consistent
     * across all simultaneously connected keyboards (e.g. built-in + external).
     * dwl uses wlr_keyboard_group for this reason. */
    struct wl_list                keyboards;  /* Keyboard::link */

    struct wlr_cursor            *cursor;
    struct wlr_xcursor_manager   *cursor_mgr;
    struct wlr_seat              *seat;

    /* Runtime configuration — loaded from ~/.evilwayrc on startup and SIGHUP.
     * All WM policy decisions (border width, terminal, keybindings) come from
     * here. Never reference TERMINAL or hardcoded keysyms directly — use this
     * struct (except Super+Shift+Q which is hardcoded as compositor emergency
     * exit and is intentionally not user-configurable). */
    EwConfig                      config;

    /* wl_event_loop signal source for SIGHUP config reload.
     * Uses signalfd internally — the callback runs in normal event loop
     * context, not from a signal handler, so malloc is safe. */
    struct wl_event_source       *sighup_source;

    /*
     * Mouse grab state — active during Super+drag move/resize operations.
     *
     * SECURITY: while cursor_mode != CurNormal ALL pointer motion events and
     * button events are consumed by the compositor and NOT forwarded to any
     * client. Enforcement is in process_cursor_motion() and
     * handle_cursor_button() in main.c.
     *
     * grab_anchor_x/y serves dual purpose:
     *   CurMove:   cursor offset from window origin at grab start
     *              (new_win_x = cursor_x - grab_anchor_x)
     *   CurResize: position of the anchored (opposite) corner in layout space
     *              (the drag corner tracks the cursor; anchor corner is fixed)
     */
    EwCursorMode      cursor_mode;
    struct Toplevel  *grab_tl;        /* window being moved/resized */
    struct wlr_box    grab_geom;      /* window geometry at grab start */
    int               grab_anchor_x;
    int               grab_anchor_y;

    /* Backend-level listeners */
    struct wl_listener new_output;
    struct wl_listener new_input;

    /* XDG shell listeners */
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;

    /* Cursor (pointer) listeners */
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    /* Seat listeners */
    struct wl_listener request_cursor;
    struct wl_listener pointer_focus_change;
    struct wl_listener request_set_selection;
};

/*
 * Output — per-display state.
 * One instance per connected monitor. Created on backend new_output event.
 */
struct Output {
    struct wl_list           link;
    struct Server           *server;
    struct wlr_output       *wlr_output;
    struct wlr_scene_output *scene_output; /* links scene graph to this output */

    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

/*
 * Toplevel — an xdg_toplevel application window.
 * One instance per window. Created on xdg_shell new_toplevel event.
 */
struct Toplevel {
    struct wl_list             link;        /* Server::toplevels */
    struct Server             *server;
    struct wlr_xdg_toplevel   *xdg_toplevel;

    /*
     * Scene graph structure for this window:
     *
     *   scene_tree (outer container, in LyrFloat)
     *   ├── border[0]   top rect,    full width × bw
     *   ├── border[1]   bottom rect, full width × bw
     *   ├── border[2]   left rect,   bw × (height - 2*bw)
     *   ├── border[3]   right rect,  bw × (height - 2*bw)
     *   └── surface_tree (xdg surface, positioned at (bw, bw))
     *
     * scene_tree is positioned at (geom.x, geom.y) in layout space.
     * scene_tree->node.data = this Toplevel, used by toplevel_at() hit test.
     *
     * DESIGN: outer tree + separate surface_tree matches dwl's c->scene /
     * c->scene_surface split. It lets us size and color borders independently
     * of the client surface without involving the xdg protocol machinery.
     */
    struct wlr_scene_tree     *scene_tree;   /* outer container; node.data = this */
    struct wlr_scene_tree     *surface_tree; /* xdg surface, at (bw,bw) in outer */
    struct wlr_scene_rect     *border[4];    /* top, bottom, left, right */

    /* Per-window mutable state: geometry, maximize flags, vdesk, fixed. */
    EwWindowState              state;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

/*
 * Popup — an xdg_popup transient window (menus, tooltips, etc.).
 * Managed by the scene graph; minimal compositor involvement needed.
 */
struct Popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

/*
 * Keyboard — per-physical-keyboard state.
 * Holds listeners for key events and the destroy event for hot-unplug.
 */
struct Keyboard {
    struct wl_list         link;   /* Server::keyboards */
    struct Server         *server;
    struct wlr_keyboard   *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

#endif /* EVILWAY_H */
