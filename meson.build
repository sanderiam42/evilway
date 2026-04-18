project('evilway', 'c',
  version: '0.2-dev',
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
# bump. Upgrading to Fedora 44 will require a migration pass.
wlroots = dependency('wlroots-0.19')

wayland = dependency('wayland-server', version: '>=1.24.0')
xkbcommon = dependency('xkbcommon')

# wayland-scanner and wayland-protocols declared for future protocol phases.
wayland_scanner = find_program('wayland-scanner')
wayland_protocols = dependency('wayland-protocols')

# SECURITY: WLR_USE_UNSTABLE is required to access wlroots scene graph,
# layer shell, session lock manager, and most compositor-relevant APIs.
# "unstable" means wlroots may change these APIs across minor versions —
# it does NOT mean the APIs are unsafe.
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

# Debug builds: address sanitizer + undefined behavior sanitizer.
# Active throughout development — not optional.
buildtype = get_option('buildtype')
if buildtype.startswith('debug')
  sanitize_args = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
  add_project_arguments(sanitize_args, language: 'c')
  add_project_link_arguments(sanitize_args, language: 'c')
endif

evilway_sources = [
  'src/main.c',
  'src/config.c',
  'src/func.c',
  'src/input.c',
  'src/output.c',
  'src/window.c',
  'src/vdesk.c',
  'src/layer.c',
  'src/lock.c',
]

executable('evilway',
  sources: evilway_sources,
  include_directories: ['include'],
  dependencies: [wlroots, wayland, xkbcommon],
  install: true,
)
