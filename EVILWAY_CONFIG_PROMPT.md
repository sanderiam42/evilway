# evilWay — Config System Prompt

Read `EVILWAY_CONTEXT.md` and `EVILWAY_CLAUDECODE_PROMPT.md` in full before starting. Then read the evilwm manual at https://www.6809.org.uk/evilwm/manual.shtml in full. The config system we are building is modeled directly on evilwm's `.evilwmrc` format. Understand it completely before writing a line of code.

---

## What we are building

A runtime configuration file parser for evilWay, reading `~/.evilwayrc` on startup and on SIGHUP. This is a deliberate departure from the dwm/dwl compile-time-only config philosophy. evilwm itself uses a runtime file and we are following evilwm's model because it is more useful for a compositor being actively tuned.

The existing compile-time `#define TERMINAL "foot"` in `evilway.h` becomes the fallback default. The runtime file overrides it. Same pattern for all configurable values.

---

## Config file format

The format is identical to evilwm's `.evilwmrc`: option names without leading dashes, one per line, arguments separated by whitespace. Comments start with `#`. Blank lines ignored.

Example (commit this as `evilwayrc.example` in the repo root):

```
# evilWay config
bw 2
snap 10
term foot

bind Super+Return=spawn
bind Super+h=move,relative+left
bind Super+Shift+h=resize,relative+left
bind Super+x=resize,toggle+v+h
bind Super+1=vdesk,0
bind Super+Left=vdesk,relative+left
bind Super+Tab=next
```

---

## Scope

Implement these config keys:

- `bw` — integer, border width in pixels, default 1
- `snap` — integer, snap-to-edge distance in pixels, default 0 (disabled)
- `term` — string, terminal to spawn, default "foot"
- `numvdesks` — integer, number of virtual desktops, default 4
- `bind` — keyboard and mouse button bindings, see grammar below

Do not implement `app` matching rules. That is a separate phase.
Do not implement color options (`fg`, `bg`, `fc`). Out of scope.
Do not implement font options (`fn`). Out of scope.

---

## The bind grammar

A bind line has this form:

```
bind TRIGGER=FUNCTION[,FLAGS]
```

TRIGGER is one of:
- `Modifier+...+Key` for keyboard binds. One or more modifiers joined by `+`, terminated by an XKB key name. Valid modifiers: `Super`, `Shift`, `Control`, `Alt`. Resolve the terminal key to an `xkb_keysym_t` using `xkb_keysym_from_name()`.
- `button1` through `button5` for mouse button binds.

FUNCTION is one of this fixed vocabulary mapping to an internal enum:
`spawn`, `delete`, `kill`, `lower`, `raise`, `move`, `resize`, `fix`, `vdesk`, `next`, `dock`, `info`

FLAGS is a `+`-joined chain of zero or more tokens from:
`relative`, `toggle`, `top`, `bottom`, `left`, `right`, `up`, `down`, `horizontal`, `vertical`

A numerical argument is also valid for `vdesk` (e.g. `vdesk,0` through `vdesk,7`).

The combination of function and flags determines runtime behavior:
- `move,relative+left` — move window incrementally left
- `move,top+left` — snap window to top-left corner
- `resize,toggle+v+h` — toggle full maximization
- `vdesk,relative+left` — switch one virtual desktop left
- `vdesk,0` — switch to desktop 0

---

## Data structures

Define these in `include/evilway.h`:

```c
/* Flags for bind actions */
#define FLAG_RELATIVE    (1 << 0)
#define FLAG_TOGGLE      (1 << 1)
#define FLAG_TOP         (1 << 2)
#define FLAG_BOTTOM      (1 << 3)
#define FLAG_LEFT        (1 << 4)
#define FLAG_RIGHT       (1 << 5)
#define FLAG_UP          (1 << 6)
#define FLAG_DOWN        (1 << 7)
#define FLAG_HORIZONTAL  (1 << 8)
#define FLAG_VERTICAL    (1 << 9)

typedef enum {
    FUNC_SPAWN, FUNC_DELETE, FUNC_KILL, FUNC_LOWER, FUNC_RAISE,
    FUNC_MOVE, FUNC_RESIZE, FUNC_FIX, FUNC_VDESK, FUNC_NEXT,
    FUNC_DOCK, FUNC_INFO
} EwFunction;

typedef struct {
    bool is_mouse;
    uint32_t modifiers;
    xkb_keysym_t keysym;
    uint32_t button;
    EwFunction function;
    uint32_t flags;
    int vdesk_target;   /* for vdesk,N — -1 if not applicable */
} EwBind;

typedef struct {
    int border_width;
    int snap_distance;
    char terminal[256];
    int num_vdesks;
    EwBind *binds;
    size_t num_binds;
    size_t binds_capacity;
} EwConfig;
```

---

## Parser implementation

Implement in `src/config.c` with header `include/config.h`.

Public interface:

```c
bool config_load(EwConfig *config);
void config_free(EwConfig *config);
void config_set_defaults(EwConfig *config);
```

Parser rules:
- Open `~/.evilwayrc` via `$HOME` env var. File absent is not an error — use defaults silently.
- Strip leading/trailing whitespace from each line.
- Skip blank lines and lines starting with `#`.
- Unknown keys: log a warning to stderr, skip, continue. Do not abort.
- Malformed bind lines: log the specific line to stderr with a parse error, skip that bind, continue. Do not abort.
- All string inputs are bounded. No unbounded `strcpy`. Use `strncpy` or `snprintf` with explicit size limits throughout.
- `SECURITY:` the config file is user-controlled input. Bound all string reads. Numeric values get range-checked: `bw` clamped 1-20, `snap` clamped 0-100, `numvdesks` clamped 1-16. Log and clamp out-of-range values, do not abort.

Write a dedicated function for bind line parsing:

```c
static bool parse_bind_line(const char *line, EwBind *bind);
```

Parse left of `=` as trigger, right as function and flags. Use `xkb_keysym_from_name()` for key resolution. Map modifier name strings to wlroots `WLR_MODIFIER_*` bitmask values.

---

## SIGHUP reload

Wire a SIGHUP handler that sets a flag. On the next compositor event loop iteration, if the flag is set: call `config_free()`, call `config_load()`, re-apply all settings, clear the flag.

Do not call `config_load()` directly from the signal handler. Signal handlers and memory allocation do not mix. Use `volatile sig_atomic_t config_reload_pending = 0` and check it in the event loop.

`SECURITY:` after reload, validate all bind entries before applying. If reload fails, log the error and continue running with the previous config intact. Never apply a partially-parsed config.

---

## Integration

- Replace the hardcoded `#define TERMINAL "foot"` with the runtime value from `EwConfig`.
- The existing Super+Return spawn keybinding becomes a default bind entry when no config file is present, not a hardcode.
- Super+Shift+Q exit bind is internal and not user-configurable. It is the compositor emergency exit. Keep it hardcoded.
- Pass `EwConfig *` to the keyboard and mouse event handlers. Bind lookup at event time: iterate `config->binds`, match on modifiers and keysym or button, dispatch the function.

---

## What NOT to do

- Do not use `system()` or `popen()` anywhere in the config or bind dispatch path.
- Do not use `getline()` with unchecked allocation — use `fgets()` with a fixed buffer.
- Do not introduce any new dependencies. Pure C stdlib plus the xkbcommon and wlroots headers already in the build.
- Do not implement `app` matching rules. Out of scope for this pass.
- Do not add color or font config options. Out of scope for this pass.

---

## Deliverables

- `src/config.c`
- `include/config.h`
- Updated `include/evilway.h` with the structs above
- Updated `src/main.c` wired to the new config system
- Updated `meson.build` to include `config.c`
- `evilwayrc.example` committed to repo root
- README updated to document the config file location, format, and SIGHUP reload
