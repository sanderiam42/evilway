# evilWay

A Wayland compositor for Fedora Asahi (aarch64) that implements the
[evilwm](https://www.6809.org.uk/evilwm/) interaction model.

This is **not a port** of evilwm. evilwm is X11-only and cannot be ported to
Wayland. evilWay is a new compositor, written from scratch in C using
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), that uses evilwm's
behavior as its specification.

## What it does (when complete)

- Floating window management only. No tiling.
- No window decorations (or minimal: 1px border).
- Keyboard-driven: all window operations via keyboard shortcuts.
- **Command key** (⌘) as the modifier — maps to Super/Mod4 under Linux on
  Apple hardware. Chosen over Alt to avoid conflicts with application shortcuts.
- Command+drag to move windows. Command+right-drag to resize.
- Focus follows mouse.
- Virtual desktops.
- No built-in status bar — [waybar](https://github.com/Alexays/Waybar) handles that.

## Current status

**Scaffold (Phase 1a):** xdg-shell only. Starts a compositor session, opens an
output, accepts xdg-shell client connections and renders them. Exits cleanly on
Super+Shift+Q. Runtime config system implemented. No evilwm window-management
behavior yet.

### Implementation roadmap

| Phase | What |
|---|---|
| 1a | xdg-shell scaffold ← **you are here** |
| 1a | ✓ VT switching (Ctrl+Alt+Fn) |
| 1a | ✓ Terminal launch (Super+Return → foot, configurable) |
| 1a | ✓ Runtime config file (~/.evilwayrc, SIGHUP reload) |
| 1b | wlr-layer-shell (waybar must work) |
| 1c | ext-session-lock-v1 (swaylock must work) |
| 1d | xdg-output, wlr-screencopy |
| 1e | XWayland |
| 2  | evilwm behavior layer on top of working compositor |
| 3  | YubiKey Bio PAM integration (pam_u2f on login/sudo/swaylock) |

## Configuration

evilWay reads `~/.evilwayrc` on startup and reloads it when sent SIGHUP.
File absent is not an error — built-in defaults are used silently.

Format is identical to evilwm's `.evilwmrc`: one option per line, no leading
dashes, arguments separated by whitespace, comments start with `#`.

### Reload

```sh
kill -HUP $(pidof evilway)
```

The compositor continues running during the reload. If the new config cannot
be parsed (malloc failure), the previous config is kept intact.

### Options

| Key | Default | Range | Description |
|---|---|---|---|
| `bw` | `1` | 1–20 | Window border width in pixels |
| `snap` | `0` | 0–100 | Snap-to-edge distance in pixels (0 = disabled) |
| `term` | `foot` | — | Terminal to spawn on the spawn bind |
| `numvdesks` | `4` | 1–16 | Number of virtual desktops |
| `bind` | — | — | Keyboard or mouse binding (see below) |

### Bind syntax

```
bind TRIGGER=FUNCTION[,FLAGS]
```

**Trigger** — keyboard:
```
bind Super+Return=spawn
bind Super+Shift+h=resize,relative+left
bind Control+Alt+t=spawn
```

**Trigger** — mouse button (button1=left, button2=middle, button3=right):
```
bind button3=lower
```

**Functions:** `spawn`, `delete`, `kill`, `lower`, `raise`, `move`, `resize`,
`fix`, `vdesk`, `next`, `dock`, `info`

**Flags** (joined with `+`): `relative`, `toggle`, `top`, `bottom`, `left`,
`right`, `up`, `down`, `horizontal` (or `h`), `vertical` (or `v`)

**vdesk** accepts either flags or a bare integer desktop number (0-based):
```
bind Super+1=vdesk,0
bind Super+Left=vdesk,relative+left
bind Super+x=resize,toggle+v+h
```

**Super+Shift+Q** is the compositor emergency exit. It is hardcoded and cannot
be removed or reassigned in the config file.

Copy `evilwayrc.example` from the repo root to `~/.evilwayrc` for a working
starting point with full documentation.

## Architecture

Built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.19, using
[dwl](https://codeberg.org/dwl/dwl) as the primary architectural reference and
tinywl (in the wlroots source tree) as the minimal floor.

## Dependencies

```
wlroots-0.19-devel
wayland-devel
wayland-protocols-devel
libxkbcommon-devel
meson
ninja-build
wayland-scanner   (usually part of wayland-devel)
```

Install on Fedora 43:

```sh
sudo dnf install wlroots-devel wayland-devel wayland-protocols-devel \
                 libxkbcommon-devel meson ninja-build
```

## Build

```sh
# Debug build (default) — includes ASan + UBSan sanitizers
meson setup build
ninja -C build

# Release build — no sanitizers
meson setup build-release --buildtype=release
ninja -C build-release
```

The debug build compiles with `-fsanitize=address,undefined`. Any memory error
or undefined behavior will be reported on stderr and the compositor will abort.
This is intentional — we run with sanitizers throughout development.

## Run

**From a TTY (primary launch path):**

Add to `~/.bash_profile`:

```sh
if [[ -z $DISPLAY && -z $WAYLAND_DISPLAY && ${XDG_VTNR:-0} -eq 1 ]]; then
    exec evilway
fi
```

Log in on TTY1. The compositor starts; failure drops back to the shell prompt.

**Nested (for development):**

```sh
# Inside an existing Wayland session:
./build/evilway
```

wlroots auto-detects the environment and opens a nested Wayland window instead
of taking over the display.

## Security notes

This compositor is the security boundary between all input and all rendered
output on the machine.

- **Socket:** The Wayland socket is created in `$XDG_RUNTIME_DIR` with 0600
  permissions. Verify on startup: `ls -la $XDG_RUNTIME_DIR/wayland-*`
- **Input:** Compositor keybindings (Super+Shift+Q, etc.) are consumed before
  any key event reaches a client.
- **Session lock:** `ext-session-lock-v1` (Phase 1c) is a hard requirement —
  swaylock uses it. The implementation must correctly inhibit all client input
  while locked, not just render a lock surface.
- **XWayland** (Phase 1e): X11 clients can keylog across windows within
  XWayland. Included for compatibility; documented where wired in.

## Privileges

Run as the session user. The `video` and `input` group memberships are required
for DRM/KMS access on Fedora — not root. Verify with `groups $(whoami)`.
