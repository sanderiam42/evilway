# evilWay — Output Management Protocol Prompt

Read `EVILWAY_CONTEXT.md` and `EVILWAY_CLAUDECODE_PROMPT.md` in full before starting.

---

## What we are building

Implement the `wlr-output-management-v1` protocol in evilWay. This allows external tools — specifically `wlr-randr` and `kanshi` — to query and configure outputs (resolution, refresh rate, scale, position, orientation) without any of that logic living inside the compositor itself. evilWay does not implement its own resolution controls. It implements the protocol and delegates to the ecosystem tooling.

This is a focused, bounded piece of work. Do not expand scope beyond what is described here.

---

## Background

evilwm had no output management — that was the X server's job. On Wayland it is the compositor's job to implement the receiving end of the output management protocol. The flow is:

```
wlr-randr or kanshi (client)
    ↓  wlr-output-management-v1 protocol
evilWay (implements protocol server side via wlroots)
    ↓  wlr_output API
KMS/DRM (kernel applies the mode change)
```

wlroots provides `wlr_output_management_v1` as a built-in. This is not a protocol we implement from scratch — we wire up the wlroots manager, listen to its events, and apply requested configurations to outputs. Keep the implementation tight. This is plumbing, not policy.

---

## Implementation

**Setup**

In the compositor initialization path, create the output management manager:

```c
server.output_manager = wlr_output_manager_v1_create(server.wl_display);
```

Wire the `apply` and `test` events:

```c
wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);
```

**The test handler**

`test` is called when a client wants to validate a configuration without applying it. Attempt the configuration in test mode using `wlr_output_commit_state()` with the test flag. Report success or failure back to the manager. Do not modify any output state. If any output in the configuration fails the test, report the entire configuration as failed.

**The apply handler**

`apply` is called when a client wants to actually apply a configuration. For each output in the configuration:
- Call `wlr_output_commit_state()` to apply the new mode, scale, transform, and position.
- If any output fails to apply, roll back all changes made so far in this apply pass and report failure.
- If all outputs succeed, report success and update the internal output layout via `wlr_output_layout`.

All-or-nothing semantics. Partial application of a multi-output configuration is not acceptable.

**Output layout sync**

After a successful apply, sync the wlroots output layout so window geometry constraints and cursor confinement reflect the new output arrangement. Any windows that end up fully off-screen after a layout change should be nudged back onto the nearest output — do not leave orphaned windows the user cannot reach.

**On new output**

When a new output is hotplugged (the `new_output` event), advertise it to the output management manager so clients can see and configure it. Use the output's preferred mode as the initial mode. Do not silently swallow new outputs.

---

## Security requirements

`SECURITY:` output configuration is a privileged operation in the sense that a malicious client could use it to make the display unusable — wrong resolution, zero scale, disabled outputs. wlroots does not authenticate clients requesting output changes. On a single-user local machine this is an acceptable risk, but document it explicitly in a comment at the apply handler. If a future phase adds any form of client authentication or sandboxing, the output management handler is on the list of operations to gate.

`SECURITY:` validate all values from the configuration before passing to wlroots. Scale must be greater than zero. Mode dimensions must be non-zero. Transform must be a valid `wl_output_transform` enum value. Log and reject any configuration containing out-of-range values before attempting apply.

`SECURITY:` the test path must be side-effect free. No output state changes, no layout changes, no logging that could be used to probe output capabilities without authorization.

---

## What NOT to do

- Do not implement custom resolution selection logic inside evilWay. That is `wlr-randr`'s job.
- Do not add any `.evilwayrc` config options for output management in this pass. Output configuration belongs to `kanshi` and its own config file (`~/.config/kanshi/config`). Do not duplicate it.
- Do not implement `wlr-output-power-management` in this pass. That is a separate protocol for display sleep states. Out of scope here.
- Do not break the existing output initialization path. The compositor must still come up correctly with no client connected and no `kanshi` running.

---

## Ecosystem context for reference

`wlr-randr` is the command-line tool for manual output configuration. Install with `dnf install wlr-randr`. Usage is analogous to `xrandr`.

`kanshi` is the tool for persistent, profile-based output configuration — different profiles for docked vs undocked, different resolutions per monitor combination. Install with `dnf install kanshi`. It reads `~/.config/kanshi/config` and applies on startup. Run it as part of the evilWay session startup, not inside the compositor.

Neither tool requires any changes. Implementing the protocol is sufficient for both to work.

---

## Deliverables

- Updated `src/main.c` with output manager initialization and event handlers
- `src/output.c` and `include/output.h` if output handling is not already in its own translation unit — if it is, add the management protocol wiring there
- Updated `meson.build` if any new source files are added
- A comment block at the top of the output management implementation explaining the all-or-nothing apply semantics and the security note about unauthenticated clients
- README updated to note `wlr-randr` and `kanshi` as the supported tools for output configuration, with install commands
