/* evilway.h — shared type definitions for the evilWay compositor
 *
 * Struct definitions and constants only. No implementation here.
 *
 * Target: wlroots-0.19 (Fedora 43: 0.19.2-1.fc43)
 */
#ifndef EVILWAY_H
#define EVILWAY_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
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

/*
 * Compositor modifier key.
 *
 * Super (Mod4) maps to the Command (⌘) key on Apple keyboards under
 * Linux/Asahi. This deliberately diverges from evilwm's Alt-based bindings:
 * Command is the natural "WM key" on this hardware and avoids conflicts
 * with terminal and application Alt shortcuts.
 *
 * TODO: Move to a config header when the behavior layer is built out.
 */
#define MODIFIER WLR_MODIFIER_LOGO

/*
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
 */
enum {
    LyrBg,      /* solid background fill */
    LyrFloat,   /* all toplevel windows — evilWay is floating-only */
    LyrTop,     /* reserved: wlr-layer-shell top/overlay surfaces (Phase 2) */
    LyrBlock,   /* reserved: ext-session-lock-v1 lock surfaces (Phase 2) */
    LYR_COUNT
};

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
    struct wlr_scene_tree     *scene_tree;  /* scene_tree->node.data = this */

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
