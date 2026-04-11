project('evilway', 'c',
  version: '0.1-dev',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
    'buildtype=debug',
  ])

cc = meson.get_compiler('c')

# wlroots-0.19 is the version shipped in Fedora 43 (0.19.2-1.fc43).
# The versioned pkg-config name is required — wlroots does not install an
# unversioned 'wlroots' pkg-config file.
#
# NOTE: Fedora 44 ships wlroots-0.20, which breaks API on every minor version
# bump. Upgrading to Fedora 44 will require a migration pass before this
# project builds cleanly.
wlroots   = dependency('wlroots-0.19')
wayland   = dependency('wayland-server', version: '>=1.24.0')
xkbcommon = dependency('xkbcommon')

wayland_scanner   = find_program('wayland-scanner')
wayland_protocols = dependency('wayland-protocols')

# ---- Protocol header generation ------------------------------------------
#
# wlroots wraps Wayland protocols but does NOT install the generated C headers
# into the system include path. As dwl's Makefile documents explicitly:
#   "wayland-scanner is a tool which generates C headers and rigging for
#    Wayland protocols. wlroots requires you to rig these up to your build
#    system yourself and provide them in the include path."
#
# Scanner modes used here (matching dwl's Makefile choices):
#   server-header  Full server-side header: listener structs, interface
#                  declarations, enums. Used for protocols we access directly.
#   enum-header    Enums only. Used for protocols wlroots wraps completely
#                  and we only need the enum constants (layer-shell, etc.).
#
# private-code (compiled C with marshaling vtables) is NOT generated here.
# wlroots already compiles protocol implementations internally; those tables
# are only needed for protocols we implement ourselves without wlroots support.
#
# To add a protocol in a later phase, append an entry to the list:
#   [scanner_mode, xml_path]
# @BASENAME@ strips the extension: 'xdg-shell.xml' → 'xdg-shell-protocol.h'

wl_proto_dir = wayland_protocols.get_variable('pkgdatadir')

protocols = [
  # Phase 1a: xdg-shell — all normal application windows
  ['server-header', join_paths(wl_proto_dir, 'stable/xdg-shell/xdg-shell.xml')],

  # Phase 1b — uncomment when wiring wlr-layer-shell (waybar):
  # ['enum-header', '<path to wlr-layer-shell-unstable-v1.xml>'],

  # Phase 1c — uncomment when wiring ext-session-lock-v1 (swaylock):
  # ['server-header', join_paths(wl_proto_dir, 'staging/ext-session-lock/ext-session-lock-v1.xml')],
]

proto_headers = []
foreach proto : protocols
  proto_headers += custom_target(
    proto[1].split('/')[-1].replace('.xml', '') + '-header',
    input:   proto[1],
    output:  '@BASENAME@-protocol.h',
    command: [wayland_scanner, proto[0], '@INPUT@', '@OUTPUT@'],
  )
endforeach

# ---- Compiler flags -------------------------------------------------------

# SECURITY: WLR_USE_UNSTABLE is required to access the wlroots scene graph,
# layer shell, session lock manager, and most compositor-relevant APIs. The
# "unstable" designation means wlroots reserves the right to change these APIs
# across minor versions — it does NOT mean the APIs are unsafe or buggy.
# Every wlroots compositor must define this macro.
add_project_arguments([
  '-DWLR_USE_UNSTABLE',
  # POSIX.1-2008: required for clock_gettime, setenv, and other POSIX functions
  # that are not visible under strict -std=c11 without an explicit feature macro.
  # dwl uses the same flag.
  '-D_POSIX_C_SOURCE=200809L',
  # wlr_log() uses the ##__VA_ARGS__ GNU extension to accept zero variadic args,
  # which is not ISO C99/C11 pedantic-clean. wlroots compositors uniformly
  # require this extension. Suppress pedantic rather than rewrite every log call.
  '-Wno-pedantic',
], language: 'c')

# Debug builds: address sanitizer + undefined behavior sanitizer.
# These are active throughout development — not optional, not an afterthought.
# Release builds drop the sanitizers but keep all warnings-as-errors.
buildtype = get_option('buildtype')
if buildtype.startswith('debug')
  sanitize_args = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
  add_project_arguments(sanitize_args, language: 'c')
  add_project_link_arguments(sanitize_args, language: 'c')
endif

# ---- Executable -----------------------------------------------------------

# Adding proto_headers to sources serves two purposes:
#   1. Build dependency: the header is generated before main.c is compiled.
#   2. Include path: meson adds the generated file's directory to the compiler
#      include path, so `#include "xdg-shell-protocol.h"` resolves correctly
#      even though the file lives in the build directory, not the source tree.
executable('evilway',
  sources: ['src/main.c', 'src/config.c', 'src/window.c'] + proto_headers,
  include_directories: ['include'],
  dependencies: [wlroots, wayland, xkbcommon],
  link_args: ['-lm'],
  install: true,
)
