# evilWay v0.2 Integration — Claude Code Prompt

---

## Context

Read `EVILWAY_CONTEXT.md` first. It is still the ground truth for platform and architecture. Nothing in it has changed.

The repo currently contains a Phase 1a scaffold: a single `src/main.c` that starts a bare compositor, renders one xdg-shell surface, and exits on Super+Shift+Q. No window management, no config, no layer-shell, no session lock, no virtual desktops.

That scaffold is being **replaced entirely** with a complete evilwm-behavior compositor. Every line of the old `src/main.c` and `include/evilway.h` is superseded. This is not a merge — it is a clean replacement. You will find the new code in a compressed tar file named `evilway-0.2-dev.tar.gz` in the repo directory. 

---

## Step 1: Create a branch

```
git checkout -b v0.2-full-implementation
```

All work happens on this branch. Do not touch `main` until the build is verified.

---

## Step 2: Remove old source files

Delete these files (they are fully superseded):

```
rm src/main.c
rm include/evilway.h
rm meson.build
rm meson_options.txt
```

Do NOT delete:
- `EVILWAY_CONTEXT.md` — project context, still accurate
- `EVILWAY_CLAUDECODE_PROMPT.md` — historical record of the Phase 1a prompt

---

## Step 3: Add new source files

Copy the following files from the provided tarball or directory into the repo, preserving the directory structure:

```
evilway/
├── meson.build
├── meson_options.txt
├── include/
│   └── evilway.h
└── src/
    ├── main.c
    ├── config.c
    ├── func.c
    ├── input.c
    ├── output.c
    ├── window.c
    ├── vdesk.c
    ├── layer.c
    └── lock.c
```

These files are the complete v0.2 codebase. Do not modify their contents during this integration step — put them in as-is.

---

## Step 4: Replace the README

Delete the current `README.md` and replace it with the content below. This is the complete new README:

---

```markdown
# evilWay

A Wayland compositor for Fedora Asahi (aarch64) that implements the
[evilwm](https://www.6809.org.uk/evilwm/) interaction model.

evilwm by Ciaran Anscomb is a minimalist X11 window manager. evilWay is a
new compositor, written from scratch in C using
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), that uses evilwm
1.5 as its behavior specification. It is not a port — evilwm is X11-only.

## What it does

- Floating window management only. No tiling.
- Minimal window decorations: 1px colored border.
- Keyboard-driven: all window operations via configurable keyboard shortcuts.
- **Super key** as the default modifier — maps to Command (⌘) on Apple
  hardware. Configurable via `.evilwayrc`.
- Super+drag to move windows. Super+middle-drag to resize.
- Focus follows mouse.
- Virtual desktops in a configurable columns×rows grid (default: 8×1).
- Snap-to-border.
- No built-in status bar — [waybar](https://github.com/Alexays/Waybar) handles
  that.
- Session lock via [swaylock](https://github.com/swaywm/swaylock)
  (`ext-session-lock-v1`).
- SIGHUP reloads configuration.

## Dependencies

```
wlroots-0.19-devel
wayland-devel
wayland-protocols-devel
libxkbcommon-devel
meson
ninja-build
```

Install on Fedora 43:

```
sudo dnf install wlroots-devel wayland-devel wayland-protocols-devel \
                 libxkbcommon-devel meson ninja-build
```

## Build

```
# Debug build (default) — includes ASan + UBSan sanitizers
meson setup build
ninja -C build

# Release build — no sanitizers
meson setup build-release --buildtype=release
ninja -C build-release
```

## Run

**From a TTY (primary launch path):**

Add to `~/.bash_profile`:

```bash
if [[ -z $DISPLAY && -z $WAYLAND_DISPLAY && ${XDG_VTNR:-0} -eq 1 ]]; then
    exec evilway
fi
```

Log in on TTY1. The compositor starts; failure drops back to the shell prompt.

**Nested (for development):**

```
./build/evilway
```

wlroots auto-detects the environment and opens a nested Wayland window.

## Configuration

evilWay reads `~/.evilwayrc` on startup. The file format is identical to
evilwm's `.evilwmrc`: one option per line, leading dashes omitted. Options
specified on the command line (if added later) override the config file.

Send SIGHUP to reload the config without restarting:

```
kill -HUP $(pidof evilway)
```

### Example `.evilwayrc`

```
# Terminal emulator (default: foot)
term foot

# Border colors (hex RGB)
fg #DAA520
fc #4682B4
bg #404040

# Border width in pixels (default: 1)
bw 1

# Snap-to-border distance in pixels (0 = disabled)
snap 10

# Virtual desktop grid layout (default: 8x1)
numvdesks 4x2

# Modifier masks (default: super for mask1 and mask2, shift for altmask)
# On Apple hardware, Command key = super.
# To use Ctrl+Alt like original evilwm:
#   mask1 control+alt
#   mask2 alt
mask1 super
mask2 super
altmask shift

# Custom key bindings (override defaults)
# Format: bind key[+modifier]...=function,flag+flag+...
# Modifiers: mask1, mask2, altmask, shift, control, alt, super, mod1-mod5
bind mask1+Return=spawn
bind mask1+altmask+q=quit

# Application rules
# Match by Wayland app_id (use wlr-which-key or similar to discover)
app firefox
vdesk 1

app foot
geometry 800x600+0+0
```

### Available options

| Option | Description |
|---|---|
| `term PROG` | Terminal to spawn (default: `foot`) |
| `fg COLOR` | Active window border color (default: `#DAA520` goldenrod) |
| `fc COLOR` | Fixed window border color (default: `#4682B4` steelblue) |
| `bg COLOR` | Inactive window border color (default: `#404040` dark grey) |
| `bw N` | Border width in pixels (default: `1`) |
| `snap N` | Snap-to-border distance, 0 to disable (default: `0`) |
| `numvdesks CxR` | Virtual desktop grid, e.g. `4x2` (default: `8x1`) |
| `wholescreen` | Ignore monitor geometry, use full screen |
| `nosoliddrag` | Draw outline instead of solid window while moving |
| `mask1 MODS` | Primary modifier (default: `super`) |
| `mask2 MODS` | Mouse/cycle modifier (default: `super`) |
| `altmask MODS` | Shift modifier for variants (default: `shift`) |
| `bind SPEC` | Key or button binding (see below) |
| `app ID` | Start an application rule block matching `app_id` |
| `geometry WxH+X+Y` | Set geometry for matched app |
| `vdesk N` | Set default vdesk for matched app |
| `fixed` | Matched app starts fixed (visible on all vdesks) |
| `dock` | Treat matched app as a dock |
| `ignore-position` | Ignore app-specified position for matched app |
| `ignore-border` | Use default border width for matched app |

### Default key bindings

All defaults use `mask1` (Super) as the primary modifier and `altmask`
(Shift) as the variant modifier.

| Key | Action |
|---|---|
| Super+Return | Spawn terminal |
| Super+Escape | Close window (co-operative) |
| Super+Shift+Escape | Kill window (force) |
| Super+Insert | Lower window |
| Super+i | Show window info (logged to stderr) |
| Super+Tab | Cycle to next window |
| Super+h/j/k/l | Move window left/down/up/right by 16px |
| Super+y/u/b/n | Move window to top-left/top-right/bottom-left/bottom-right corner |
| Super+Shift+h/j/k/l | Resize window (shrink left, grow down, shrink up, grow right) |
| Super+= | Toggle vertical maximize |
| Super+Shift+= | Toggle horizontal maximize |
| Super+x | Toggle full maximize (both axes) |
| Super+d | Toggle dock visibility |
| Super+f | Toggle window fixed (visible on all vdesks) |
| Super+1–8 | Switch to virtual desktop 0–7 |
| Super+Left/Right/Up/Down | Navigate virtual desktop grid |
| Super+a | Switch to previously active virtual desktop |
| Super+Shift+q | Quit compositor |

### Default mouse bindings

With Super held, anywhere in a window:

| Button | Action |
|---|---|
| Left | Move window |
| Middle | Resize window |
| Right | Lower window |

### Key repeat

Holding a compositor key binding continuously repeats the action. This
applies to relative move (h/j/k/l), relative resize (Shift+h/j/k/l),
and relative vdesk navigation (arrow keys). Initial delay: 400ms, repeat
rate: 30ms.

### Bindable functions

| Function | Flags | Description |
|---|---|---|
| `spawn` | — | Launch terminal |
| `delete` | — | Co-operative close |
| `kill` | — | Force kill |
| `lower` | — | Lower window |
| `raise` | — | Raise window |
| `next` | — | Cycle windows |
| `move` | `relative+{left,right,up,down}` | Move by step |
| `move` | `{top,bottom}+{left,right}` | Move to corner |
| `resize` | `relative+{left,right,up,down}` | Resize by step |
| `resize` | `toggle+{v,h,v+h}` | Toggle maximize |
| `fix` | `toggle` | Toggle fixed state |
| `dock` | `toggle` | Toggle dock visibility |
| `info` | — | Show window info |
| `vdesk` | `N` (number) | Switch to vdesk N |
| `vdesk` | `relative+{left,right,up,down}` | Navigate vdesk grid |
| `vdesk` | `toggle` | Switch to previous vdesk |
| `quit` | — | Exit compositor |

## Architecture

Built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.19.
[dwl](https://codeberg.org/dwl/dwl) was used as the primary architectural
reference and tinywl as the minimal floor.

### Source layout

```
src/main.c      — server init, scene graph, signal handling, event loop
src/config.c    — .evilwayrc parser, default binds, app rules
src/func.c      — all bindable functions (evilwm-compatible dispatch)
src/input.c     — keyboard, pointer, focus-follows-mouse, key repeat
src/window.c    — view lifecycle, borders, app rule matching
src/output.c    — display management via wlr_scene
src/vdesk.c     — virtual desktops (NxM grid)
src/layer.c     — wlr-layer-shell-v1 (waybar, panels)
src/lock.c      — ext-session-lock-v1 (swaylock)
include/evilway.h — all structs, enums, prototypes
```

### Wayland protocols

| Protocol | Purpose |
|---|---|
| xdg-shell | Normal application windows |
| wlr-layer-shell-v1 | waybar, wofi, swaylock surfaces |
| ext-session-lock-v1 | Session lock (swaylock) |

XWayland is disabled by default (meson option). Layer-shell and session
lock are built in and always available.

### Scene graph layers (bottom to top)

1. Background
2. Bottom
3. Views (normal windows)
4. Top
5. Overlay
6. Lock (session lock surface — topmost)

## Security notes

This compositor is the security boundary between all input and all
rendered output.

- **Input**: compositor keybindings are consumed before reaching clients.
  No client can intercept Super+Shift+Q or the lock invocation.
- **Session lock**: when locked, ALL input is routed only to the lock
  surface. If the lock surface is destroyed without unlocking, the session
  stays locked. There is no compositor bypass. Recovery is via TTY.
- **Socket**: Wayland socket is created with 0600 permissions in
  `$XDG_RUNTIME_DIR`.
- **Sanitizers**: debug builds run with `-fsanitize=address,undefined`.
  All development should use debug builds.

## Privileges

Run as the session user. Requires `video` and `input` group membership
for DRM/KMS access. Not root.

```
groups $(whoami)    # should include video, input
```

## Credits

- [evilwm](https://www.6809.org.uk/evilwm/) by Ciaran Anscomb — the
  behavior specification
- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — the
  compositor library
- [dwl](https://codeberg.org/dwl/dwl) — architectural reference
- [tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl)   — minimal wlroots example
---

## Step 5: Commit and push
```
git add -A
git commit -m "v0.2: full evilwm behavior implementation
```

Replace Phase 1a scaffold with complete compositor implementing all
evilwm 1.5 features:

- .evilwayrc config file (identical syntax to .evilwmrc)
- All 12 bindable functions: spawn, delete, kill, lower, raise, next,
  move (relative + corner), resize (relative + toggle maximize),
  fix, dock, info, vdesk
- Virtual desktops with NxM grid navigation
- Focus follows mouse
- Key repeat on held compositor bindings
- wlr-layer-shell-v1 (waybar support)
- ext-session-lock-v1 (swaylock support)
- Application matching rules
- Snap-to-border
- SIGHUP config reload
- Configurable modifier masks (default: Super for Apple hardware)
- 4-rect border rendering with active/fixed/inactive colors

~2800 lines of C across 10 files. Architecture follows dwl patterns
on wlroots 0.19 scene-tree API."

git push origin v0.2-full-implementation

---

## What NOT to do

- Do not attempt to merge old `src/main.c` with new code. There is zero overlap. Replace, don't merge.
- Do not attempt to build or compile on this system. All the building and compiling will be done on the Asahi system and then the results will be given to you to work with. 
- Do not delete `EVILWAY_CONTEXT.md` or `EVILWAY_CLAUDECODE_PROMPT.md`. They are historical documentation.
- Do not modify the new source files during integration unless fixing compilation errors. Design changes come after the branch lands.
- Do not add XWayland in this step. It stays disabled.
