# evilWay — Scale Bug Fix Prompt

Read `EVILWAY_CONTEXT.md` and `EVILWAY_CLAUDECODE_PROMPT.md` before starting.

There is a scaling bug introduced by `wlr-randr --output eDP-1 --scale 2`. Symptoms:

- Firefox window borders only visible on top and left edges
- Click-to-focus only works inside roughly the top-left quadrant of the firefox window
- foot terminals are unaffected

The diagnosis: evilWay is tracking window geometry in buffer pixels rather than logical (pre-scale) coordinates. At scale 2, a window's buffer is 2x its logical size. evilWay thinks the window ends at the midpoint of its actual rendered area. Border drawing and pointer hit-testing both use this incorrect geometry.

The fix: audit every place in the codebase that reads window dimensions or position and confirm it is using logical coordinates, not buffer pixel dimensions. Specifically:

- Border drawing must use logical geometry
- Pointer focus hit-testing must use logical geometry
- Window move and resize must operate in logical coordinates
- Input region routing through `wlr_seat` must use logical coordinates

In wlroots, logical coordinates come from `wlr_xdg_surface` geometry and `wlr_scene` node positions. Buffer dimensions come from `wlr_surface` buffer width/height. These are different things. The scale factor lives at the output layer — wlroots handles buffer-to-output scaling internally. The compositor should never need to multiply or divide by the scale factor manually. If you find any place in the code doing that, it is wrong.

`SECURITY:` input routing using incorrect geometry means clicks land in the wrong window. At worst this means a user believes they are interacting with one window while input is going to another. Confirm the fix is complete by verifying the full window surface area receives focus and pointer events correctly at scale 2.

Do not change anything related to output management, wlr-randr integration, or scale factor setting. The scale is correct. The window geometry handling is what needs fixing.

Call out every location you change and explain why that location was using buffer coordinates instead of logical coordinates.
