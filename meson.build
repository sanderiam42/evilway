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

# wayland-scanner and wayland-protocols are not used in the xdg-shell scaffold
# (wlroots wraps xdg-shell completely). They are declared here now because
# every subsequent protocol phase (layer-shell, session-lock, etc.) will need
# them and it is easier to add them once.
wayland_scanner   = find_program('wayland-scanner')
wayland_protocols = dependency('wayland-protocols')

# SECURITY: WLR_USE_UNSTABLE is required to access the wlroots scene graph,
# layer shell, session lock manager, and most compositor-relevant APIs. The
# "unstable" designation means wlroots reserves the right to change these APIs
# across minor versions — it does NOT mean the APIs are unsafe or buggy.
# Every wlroots compositor must define this macro.
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

# Debug builds: address sanitizer + undefined behavior sanitizer.
# These are active throughout development — not optional, not an afterthought.
# Release builds drop the sanitizers but keep all warnings-as-errors.
buildtype = get_option('buildtype')
if buildtype.startswith('debug')
  sanitize_args = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
  add_project_arguments(sanitize_args, language: 'c')
  add_project_link_arguments(sanitize_args, language: 'c')
endif

executable('evilway',
  sources: ['src/main.c'],
  include_directories: ['include'],
  dependencies: [wlroots, wayland, xkbcommon],
  install: true,
)
