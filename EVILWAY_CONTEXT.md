# evilWay: Project Context

## What This Is

A new Wayland compositor written in C, using wlroots, that implements the evilwm interaction model. Not a port — evilwm is X11-only and cannot be ported. This is a ground-up wlroots compositor with evilwm as the behavior specification.

evilwm behavior spec: https://www.6809.org.uk/evilwm/
wlroots: https://gitlab.freedesktop.org/wlroots/wlroots
Primary reference implementation: https://codeberg.org/dwl/dwl (dwm semantics on wlroots, ~2500 lines of C)
Minimal skeleton reference: tinywl (ships inside wlroots source tree, ~500 lines)

## Target Platform

- MacBook Pro 14-inch Early 2023 (MPHE3LL/A), Apple M2 Pro
- Fedora Asahi Remix (currently Fedora 43 base), aarch64
- 16K kernel page size — affects userspace binaries, not compositor code
- Wayland-only platform. Xorg not supported beyond bare essentials on Asahi.
- Sway already runs on this hardware, proving wlroots works on this exact platform.

## Session Architecture

- **No display manager.** Boot goes to TTY `login:` prompt.
- evilWay launches from shell rc on TTY1 (e.g. `~/.bash_profile`: `if [[ -z $DISPLAY && $XDG_VTNR -eq 1 ]]; then exec evilway; fi`)
- Failure condition is clean: compositor crash or failed start drops back to TTY prompt with full system access.
- Register as a Wayland session anyway (`/usr/share/wayland-sessions/evilway.desktop`) for future optionality, but TTY is the primary launch path during development.

## Authentication Stack

### Phase 1 (current)
Plain password auth throughout. TTY login with standard password. sudo with standard password. swaylock with standard password. No YubiKey involvement yet.

### Phase 2 (later, after compositor is stable and daily-drivable)
- YubiKey Bio C (USB-C) added as hardware root of trust.
- `pam_u2f` layered in against:
  - `/etc/pam.d/login` — TTY login
  - `/etc/pam.d/sudo` — privilege escalation
  - `/etc/pam.d/swaylock` — mid-session lock screen
- PAM stack is compositor-agnostic. Works identically whether session started via SDDM or TTY.
- **PAM misconfiguration warning:** when editing PAM files in Phase 2, keep a root shell open in a second TTY the entire time. Do not close it until the new config is verified working. Misconfigured PAM can lock you out of sudo and login with no recovery path short of booting to recovery mode.

### swaylock dependency
swaylock uses the `ext-session-lock-v1` Wayland protocol regardless of auth backend. evilWay must implement this protocol for swaylock to function at all — this is a Phase 1 requirement. The YubiKey PAM integration in Phase 2 layers on top of a working swaylock, it does not change the protocol requirement.

## Wayland Protocols Required

These are the non-negotiable protocols for the target application stack:

| Protocol | Why |
|---|---|
| `xdg-shell` | All normal application windows |
| `wlr-layer-shell` | waybar, wofi popups, swaylock |
| `ext-session-lock-v1` | swaylock — mandatory for YubiKey lock screen chain |
| `xdg-output` | Multi-monitor, output introspection |
| `wlr-screencopy` | Screenshots, screen sharing |

## XWayland

Include XWayland from the start. wlroots makes this straightforward. Cost of inclusion is low; cost of retrofitting is annoying.

## Application Compatibility Notes

- **Firefox**: works on any wlroots compositor via xdg-shell. No issues.
- **Chromium/Chrome**: same. Watch for GPUCache corruption at `~/.config/<app>/GPUCache` after updates — delete cache dir to fix. Compositor-independent issue.
- **waybar**: needs `wlr-layer-shell`. Confirmed working on wlroots compositors.
- **wofi / alacritty / foot**: standard xdg-shell clients. No special requirements.
- **swaylock + swayidle**: needs `ext-session-lock-v1` and `wlr-layer-shell`.
- **GNOME Keyring**: do not start it. Not needed. No GNOME dependencies in this stack.
- **Bitwarden desktop**: no ARM64 Linux build exists yet (PR #14270 in progress as of April 2025). Living in browser extension only. Compositor-independent.
- **1Password desktop**: works on ARM64 Linux since 8.10.0.
- **VS Code**: ARM64 build works, install via Microsoft RPM repo.
- **Claude Code**: Node.js CLI, bundles linux-arm64 binaries, works fine.

## Platform Constraints That Do NOT Affect evilWay

- Thunderbolt WIP — ports work as USB-C only. Not relevant.
- DP Alt Mode WIP — no USB-C display output. Use native HDMI port. Not relevant to compositor code.
- TouchID / SEP non-functional — YubiKey replaces this entirely. Not relevant.
- Video decoder WIP — software decode only. Not relevant to compositor.
- 16K page size — userspace binary issue, not a compositor build issue.

## evilwm Behavior Specification (what evilWay must replicate)

Core interaction model from evilwm:
- Floating window management only. No tiling.
- No window decorations by default (or minimal: 1px border).
- Keyboard-driven: all window operations via keyboard shortcuts.
- Mouse: Alt+drag to move, Alt+right-drag to resize.
- Focus follows mouse (configurable).
- No config file in the traditional sense — behavior baked in, keybindings defined at compile time via `#define` or a single header.
- No status bar built in — external bar (waybar) handles that.
- Snap-to-edge behavior.
- Fixed-size stepping for keyboard-driven resize.
- Virtual desktops (evilwm calls them "virtual screens").

## Development Approach

1. Start from tinywl. Understand the wlroots scene-tree API before writing any window management logic.
2. Study dwl source before writing anything. It's the closest working reference.
3. Implement protocol list above before touching window management policy.
4. Window management policy (the evilwm behavior) comes last, on top of a working compositor skeleton.
5. Test each protocol implementation against its target application before moving on.

## Key wlroots Concepts to Internalize First

- Scene-tree API (wlr_scene) — the right rendering abstraction, don't bypass it
- `wlr_xdg_shell` for window lifecycle
- `wlr_seat` for input handling
- `wlr_output_layout` for display management
- `wlr_layer_shell_v1` for overlay surfaces (bar, lock screen)
- `wlr_session_lock_manager_v1` for ext-session-lock-v1

## Language and Build

- C. Not Rust, not C++. Matches evilwm lineage and wlroots' own language.
- Meson build system (wlroots uses it, dwl uses it, follow the ecosystem).
- Keep it small. If the line count is growing past dwl without good reason, something is wrong.
