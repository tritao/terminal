project(
  'pragtical-terminal',
  'c',
  version: '1.0.0',
  meson_version: '>= 0.63',
  default_options: ['c_std=gnu11', 'warning_level=3', 'werror=true']
)

cc = meson.get_compiler('c')
fs = import('fs')

# When embedded in Pragtical, the parent project provides the plugin API as a
# dependency. Standalone builds use the checked-out Pragtical source tree.
plugin_api_dep = dependency('pragtical_plugin_api', required: false)
if not plugin_api_dep.found()
  api_include_dir = get_option('api_include_dir')
  if api_include_dir == ''
    api_include_dir = 'lib/pragtical/resources/include'
  endif
  if not fs.is_file(api_include_dir / 'pragtical_plugin_api.h')
    error('Pragtical plugin API not found; set -Dapi_include_dir=...')
  endif
  plugin_api_dep = declare_dependency(
    include_directories: include_directories(api_include_dir)
  )
endif

terminal_dependencies = [plugin_api_dep]
if host_machine.system() != 'windows'
  terminal_dependencies += cc.find_library('util', required: true)
endif

terminal_native_dependencies = []
if host_machine.system() != 'windows'
  terminal_native_dependencies += cc.find_library('util', required: true)
endif

terminal_emulator_source = 'native/emulator/terminal_emulator.c'
terminal_emulator_dependencies = terminal_native_dependencies
if get_option('emulator_backend') == 'libtsm'
  if get_option('wrap_mode') == 'forcefallback'
    # The fallback is bundled into the native plugin so the plugin remains
    # self-contained in portable and test runtimes. A system libtsm remains
    # selectable through the normal dependency lookup.
    libtsm_project = subproject(
      'libtsm',
      default_options: [
        'default_library=static',
        'tests=false',
        'gtktsm=false',
      ],
    )
    libtsm_dep = libtsm_project.get_variable('libtsm_dep')
  else
    libtsm_dep = dependency(
      'libtsm',
      version: '>=4.7.1',
      required: get_option('wrap_mode') == 'nofallback',
    )
    if not libtsm_dep.found()
      libtsm_project = subproject(
        'libtsm',
        default_options: [
          'default_library=static',
          'tests=false',
          'gtktsm=false',
        ],
      )
      libtsm_dep = libtsm_project.get_variable('libtsm_dep')
    endif
  endif
  terminal_emulator_source = 'native/emulator/terminal_emulator_libtsm.c'
  terminal_emulator_dependencies += libtsm_dep
endif

terminal_emulator_library = static_library(
  'pragtical-terminal-emulator',
  terminal_emulator_source,
  dependencies: terminal_emulator_dependencies,
  install: true
)

terminal_emulator_dep = declare_dependency(
  link_with: terminal_emulator_library,
  include_directories: include_directories('native/emulator'),
  dependencies: terminal_emulator_dependencies
)

terminal_runtime_library = static_library(
  'pragtical-terminal-runtime',
  'native/runtime/terminal_runtime.c',
  dependencies: terminal_native_dependencies,
  install: true
)

terminal_runtime_dep = declare_dependency(
  link_with: terminal_runtime_library,
  include_directories: include_directories('native/runtime')
)

terminal_core_library = static_library(
  'pragtical-terminal-core',
  'native/terminal_core.c',
  dependencies: [terminal_emulator_dep, terminal_runtime_dep],
  install: true
)

terminal_core_dep = declare_dependency(
  link_with: terminal_core_library,
  dependencies: [terminal_emulator_dep, terminal_runtime_dep],
  include_directories: include_directories('native')
)

native_components_test_args = []
if get_option('emulator_backend') == 'libtsm'
  native_components_test_args += '-DTERMINAL_EMULATOR_LIBTSM'
endif

native_components_test = executable(
  'terminal-native-components-test',
  'tests/native_components.c',
  include_directories: include_directories('native'),
  link_with: [terminal_core_library, terminal_emulator_library, terminal_runtime_library],
  dependencies: terminal_emulator_dependencies,
  c_args: native_components_test_args
)
test('terminal-native-components', native_components_test)

if get_option('codex_pty_test')
  if get_option('emulator_backend') != 'libtsm'
    error('codex_pty_test requires the libtsm emulator backend')
  endif
  codex_pty_test = executable(
    'terminal-codex-pty-test',
    'tests/codex_pty.c',
    include_directories: include_directories('native'),
    link_with: [terminal_emulator_library, terminal_runtime_library],
    dependencies: terminal_emulator_dependencies,
    c_args: ['-DTERMINAL_EMULATOR_LIBTSM']
  )
  test('codex-pty', codex_pty_test, timeout: 240)
endif

install_headers(
  'native/terminal_core.h',
  subdir: 'pragtical-terminal'
)
install_headers(
  'native/emulator/terminal_emulator.h',
  subdir: 'pragtical-terminal/emulator'
)
install_headers(
  'native/runtime/terminal_runtime.h',
  subdir: 'pragtical-terminal/runtime'
)

pragtical_native_plugin_module = 'plugins.terminal.libterminal'
pragtical_native_plugin_root = meson.current_source_dir()
pragtical_native_plugin_target = shared_module(
  'terminal',
  'src/libterminal.c',
  dependencies: terminal_dependencies + [terminal_core_dep],
  # The entrypoint includes the bundled Pragtical API header, which may be
  # older than the header used by an embedding checkout. Keep this boundary
  # at -Wall -Wextra while native terminal code is checked with -Wpedantic.
  override_options: ['warning_level=2'],
  name_prefix: 'lib',
  install: true,
  install_dir: get_option('data_dir') / 'plugins' / 'terminal'
)

install_data(
  'plugins/terminal/init.lua',
  'plugins/terminal/session.lua',
  'plugins/terminal/emulator.lua',
  'plugins/terminal/local_backend.lua',
  install_dir: get_option('data_dir') / 'plugins' / 'terminal'
)
