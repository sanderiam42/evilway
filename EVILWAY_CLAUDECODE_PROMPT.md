# evilWay — Claude Code Kickoff Prompt

---

Read EVILWAY_CONTEXT.md in full before doing anything. That document is the ground truth for platform, architecture, and behavioral requirements. Do not proceed without reading it.

---

## What we are building

A Wayland compositor in C called evilWay. It is not a port of evilwm — evilwm is X11-only and cannot be ported. evilWay is a new wlroots-based compositor that uses evilwm as its behavior specification: floating window management, keyboard-driven, minimal decoration, mouse Alt+drag, focus-follows-mouse, virtual desktops.

## Your job right now

Scaffold the project. No window management logic yet. I want a building, compiling, running skeleton that proves the wlroots foundation is correct before we write a single line of evilwm behavior.

## Step 1: Establish references

Before writing any code, do the following and report back:

1. Clone dwl from https://codeberg.org/dwl/dwl and read its source. Clone to /tmp, not inside the project tree — these are reference reads only, not dependencies. Specifically: `dwl.c`, `meson.build`, and the protocol XML files it pulls in. This is our closest working reference. Note which wlroots APIs it uses for each of: window lifecycle, input handling, output management, layer shell, session lock.

2. Locate tinywl in the wlroots source tree (it lives under `tinywl/` in the wlroots repo at https://gitlab.freedesktop.org/wlroots/wlroots). Clone to /tmp alongside dwl. Read it. This is the floor — the absolute minimum wlroots compositor. Note what it omits that we will need.

3. Check the current wlroots version available on Fedora Asahi Remix (aarch64, Fedora 43): `dnf info wlroots` and `dnf info wlroots-devel`. Report the version. We will build against whatever is in the Fedora repo unless there's a compelling reason not to.

Do not write project code until you have done this and we have discussed what you found.

## Step 2: Scaffold (after Step 1 discussion)

Project structure:
```
evilway/
├── meson.build
├── meson_options.txt
├── src/
│   └── main.c
├── protocols/           # wayland protocol XML files, fetched from upstream
└── include/
    └── evilway.h
```

The scaffold must:
- Build cleanly with meson + ninja on Fedora Asahi Remix aarch64
- Link against wlroots and wayland-server
- Start a Wayland compositor session
- Open an output (display)
- Accept a single client connection and render it (xdg-shell surface)
- Exit cleanly on Mod+Shift+Q (we will change the modifier mapping later — for now hardcode)
- Log to stderr, verbose during development

It must NOT yet:
- Implement any evilwm window management behavior
- Implement layer-shell
- Implement session lock
- Implement XWayland
- Have any config system

We add those in order, one protocol at a time, testing each against its target application before moving on. The order will be:
1. xdg-shell skeleton (Step 2, this step)
2. wlr-layer-shell (waybar must work)
3. ext-session-lock-v1 (swaylock must work — non-negotiable even in Phase 1; YubiKey PAM layers on top of this in Phase 2 without changing the protocol requirement)
4. xdg-output
5. wlr-screencopy
6. XWayland
7. evilwm behavior layer on top of working compositor

## Security requirements

Security is a first-class design constraint, not a later audit. This compositor sits between all input and all rendered output on the machine. Mistakes here are not cosmetic.

**Input handling**
- All input from `wlr_seat` must be validated before dispatch. Do not assume well-formed input from clients.
- Keyboard shortcuts must be consumed by the compositor before being passed to clients. Verify this explicitly — a client that can intercept Mod+Shift+Q or the lock screen invocation is a security hole.
- `wlr_seat` pointer and keyboard grabs must be released cleanly on client exit. Stuck grabs can freeze input system-wide.

**Session lock**
- `ext-session-lock-v1` implementation must be correct and complete before the lock screen is considered functional. A compositor that renders a lock surface but does not actually inhibit input to other clients is not a lock screen.
- Test explicitly: with swaylock running, verify that no other client receives keyboard or pointer events.
- Do not implement any compositor-side "emergency unlock" shortcut. If you need to recover from a broken lock screen, that's what the Phase 2 YubiKey fallback and the TTY root shell are for.

**IPC and socket**
- wlroots compositors expose a Wayland socket. Confirm the socket is created with correct permissions (0600 or equivalent, owned by the session user). Verify this — do not assume wlroots defaults are correct for your use case.
- If we add any compositor IPC beyond the Wayland protocol (e.g. a control socket for scripting), it gets the same socket permission treatment. No world-readable sockets.

**XWayland**
- XWayland is a significant attack surface. It runs as a separate process with its own socket. Confirm XWayland socket permissions are not world-accessible.
- X11 clients under XWayland can do things Wayland clients cannot (keylogging across windows is the classic example). We are including XWayland for compatibility, not because we trust it. Note this explicitly in comments wherever XWayland is wired in.
- If an XWayland client misbehaves, the blast radius should be contained to XWayland. Verify the wlroots XWayland isolation model before wiring it in.

**Memory safety**
- This is C. Use address sanitizer (`-fsanitize=address`) and undefined behavior sanitizer (`-fsanitize=undefined`) during development builds. Wire these into the meson debug build configuration from day one, not as an afterthought.
- No unchecked return values from wlroots API calls that return nullable pointers. Every `wlr_*` call that can return NULL gets a NULL check with a logged error and clean exit or recovery path.
- No use-after-free on client destruction. wlroots signals client destruction events — handle them, do not let stale references persist.

**Privileges**
- The compositor should run as the session user. No setuid, no capabilities beyond what wlroots requires for DRM/KMS access.
- Confirm what privileges wlroots actually needs on Fedora Asahi — `video` and `input` group membership is the usual answer, not root. Document it.

**Build**
- Debug builds: `-fsanitize=address,undefined`, full warnings (`-Wall -Wextra -Werror`).
- Release builds: remove sanitizers, keep warnings-as-errors.
- Both build types defined in meson from the start.

When you encounter a design decision that has security implications, flag it explicitly with a `SECURITY:` comment in both the code and your response. I want to see those forks.


- C only. No C++. No Rust. Matches wlroots and evilwm lineage.
- Meson build system. Not CMake, not autotools.
- Keep it small. dwl is ~2500 lines. tinywl is ~500. We should not exceed dwl's line count meaningfully given our simpler (floating-only) window model.
- No unnecessary abstraction. This is a compositor, not a framework.
- Comments on non-obvious wlroots API usage. The API has sharp edges; document the ones we hit.
- When you make a design decision that has alternatives, call it out explicitly as a decision with the rationale. I want to know where the forks were.

## What I want back from Step 1

A summary structured as:
- wlroots version on Fedora 43 aarch64
- dwl: which wlroots APIs map to which functional areas
- dwl: anything in dwl we definitely do NOT need (tiling logic, etc.)
- tinywl: what it omits that is on our required protocol list
- Any surprises, version mismatches, or API deprecations worth knowing before we write code
- Your recommendation on whether to build against system wlroots or pin to a specific version

Do not write project scaffolding until we have had that conversation.
