# evilWay — Window Movement and Resize Prompt

Read `EVILWAY_CONTEXT.md` and `EVILWAY_CLAUDECODE_PROMPT.md` in full before starting. Then read the evilwm manual at https://www.6809.org.uk/evilwm/manual.shtml, specifically the USAGE and FUNCTIONS sections. This prompt implements the move and resize behavior described there, adapted for Wayland and wired to the bind system already in place via `.evilwayrc`.

---

## What we are building

The move and resize behavior layer. This is the core of what makes evilWay feel like evilwm. There are two input paths for both move and resize: keyboard and mouse. Both must be implemented. Both must dispatch through the existing `EwBind` / `EwConfig` system — no hardcoded behavior outside the bind dispatch path.

---

## Keyboard move and resize

The bind system already parses `move` and `resize` with their flags. What does not exist yet is the implementation of those functions when dispatched.

**move,relative+[left|right|up|down]**

Move the focused window by a fixed pixel increment in the specified direction. The increment is 16 pixels, matching evilwm's default. Make this a named constant:

```c
#define MOVE_STEP 16
```

Moving must not push a window fully off-screen. Clamp to keep at least `MOVE_STEP` pixels of the window visible on the output in each direction.

**move,[top|bottom]+[left|right]**

Move the focused window to a corner of the current output. The four combinations are top+left, top+right, bottom+left, bottom+right. Position the window flush to the corner accounting for border width.

**resize,relative+[left|right|up|down]**

Resize the focused window by a fixed pixel increment. evilwm's model: `up` reduces height, `down` increases height, `left` reduces width, `right` increases width. Use the same step constant as move. Enforce a minimum window size — do not allow width or height below 32 pixels.

**resize,toggle+[v|h] and resize,toggle+v+h**

Toggle maximization states:
- `toggle+v` — maximize vertically on current output, restore on second press
- `toggle+h` — maximize horizontally on current output, restore on second press
- `toggle+v+h` — maximize both axes (full screen minus borders), restore on second press

Store pre-maximization geometry per window so restore works correctly. A window maximized vertically then horizontally should restore to its original geometry, not an intermediate state.

---

## Mouse move and resize

evilwm's mouse model: Super+drag anywhere in the window to move, Super+right-drag anywhere in the window to resize. This is not click-on-border dragging. The modifier is held and the drag happens on the window surface itself.

In wlroots terms this means:
- On pointer button press with Super held: begin an interactive move or resize using `wlr_scene_node` position manipulation
- On pointer motion during active grab: update window position or size
- On pointer button release: end the grab, clean up state

Use wlroots' built-in interactive move/resize support. Look at how dwl implements `moveresize` in `dwl.c` — it is the correct reference for this on wlroots. Clone dwl to /tmp for reference, do not add it as a dependency.

**Move:** Super+button1 drag. Update window position to follow pointer delta.

**Resize:** Super+button2 drag (evilwm uses button2 not button3 for resize). Resize from the nearest corner to the pointer — determine which corner is closest at drag start and anchor the opposite corner.

**Lower:** Super+button3 — lower the window in the stacking order. No drag involved, just a click.

Mouse binds must also be configurable via `.evilwayrc` using the existing `bind button1=move` syntax. The defaults above should be the fallback when no mouse binds are present in the config file.

`SECURITY:` during an active mouse grab, the compositor must consume all pointer events and not forward them to any client until the grab is released. A client that receives pointer events during a compositor grab is a security and correctness bug. Verify this explicitly in the implementation.

---

## Focus model

evilwm uses focus-follows-mouse. Implement it now as it is required for move and resize to feel correct.

- When the pointer enters a window, give that window keyboard focus.
- When the pointer moves to the root/background, do not transfer focus — retain the last focused window. This matches evilwm's behavior exactly.
- Raise on focus is not evilwm's default. Do not raise on focus unless a future config option enables it.
- The focused window gets the active border color. Unfocused windows get the inactive border color. Both are constants for now:

```c
#define BORDER_COLOR_ACTIVE   0xDAA520FF  /* evilwm gold */
#define BORDER_COLOR_INACTIVE 0x444444FF
```

These will become config options in a later pass when color config is added. For now they are compile-time constants.

---

## Window stacking

evilwm is a floating WM. Windows have a stacking order. Implement the following:

- Clicking anywhere in a window raises it to the top of the stack.
- `lower` function (from bind dispatch) lowers the window to the bottom.
- `raise` function raises the window to the top.
- Fixed windows (marked with `fix,toggle`) are always visible regardless of virtual desktop — stacking order among fixed windows still applies.

Use the wlroots scene graph for stacking. `wlr_scene_node_raise_to_top()` and `wlr_scene_node_lower_to_bottom()` are the relevant calls.

---

## Snap to edge

The `snap` config value is already parsed. Wire it in here.

When a window is moved via keyboard or mouse and comes within `snap` pixels of an output edge or another window edge, snap it flush to that edge. If `snap` is 0, disable snapping entirely.

Snap applies to:
- Output edges (all four)
- Other window edges (all four edges of each other visible window)

Do not snap to windows on other virtual desktops.

---

## What NOT to do

- Do not implement virtual desktops in this pass. The vdesk bind functions exist in the enum but dispatch to a stub that logs "vdesk not yet implemented" to stderr.
- Do not implement the `info` function in this pass. Stub it the same way.
- Do not implement `dock` toggle in this pass. Stub it.
- Do not add any window decorations beyond the single-pixel colored border. No titlebars, no close buttons, nothing.
- Do not implement `wlr-layer-shell` in this pass. That is a separate phase.

---

## Deliverables

- Updated `src/main.c` with focus-follows-mouse, stacking, and bind dispatch for all move/resize functions
- `src/window.c` and `include/window.h` — extract window state management into its own translation unit. Per-window state includes: current geometry, pre-maximization geometry, maximization flags, fixed flag, virtual desktop number (stored but not acted on yet).
- Updated `meson.build` to include `window.c`
- All new functions annotated with `SECURITY:` comments where input from clients or config affects behavior
- Design decision callouts wherever there were real alternatives considered
