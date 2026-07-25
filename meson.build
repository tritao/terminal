project(
  'pragtical-terminal',
  'c',
  version: '1.0.0',
  meson_version: '>= 0.63',
  default_options: ['c_std=gnu11']
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

terminal_core_dependencies = []
if host_machine.system() != 'windows'
  terminal_core_dependencies += cc.find_library('util', required: true)
endif

terminal_core_library = static_library(
  'pragtical-terminal-core',
  'native/terminal_core.c',
  dependencies: terminal_core_dependencies,
  install: true
)

terminal_core_dep = declare_dependency(
  link_with: terminal_core_library,
  include_directories: include_directories('native')
)

terminal_emulator_library = static_library(
  'pragtical-terminal-emulator',
  'native/emulator/terminal_emulator.c',
  dependencies: terminal_core_dep,
  install: true
)

terminal_runtime_library = static_library(
  'pragtical-terminal-runtime',
  'native/runtime/terminal_runtime.c',
  dependencies: terminal_core_dep,
  install: true
)

native_components_test = executable(
  'terminal-native-components-test',
  'tests/native_components.c',
  include_directories: include_directories('native'),
  link_with: [terminal_core_library, terminal_emulator_library, terminal_runtime_library],
  dependencies: terminal_core_dependencies
)
test('terminal-native-components', native_components_test)

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
