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

shared_module(
  'terminal',
  'src/libterminal.c',
  dependencies: terminal_dependencies,
  name_prefix: 'lib',
  install: true,
  install_dir: get_option('data_dir') / 'plugins' / 'terminal'
)

install_data(
  'plugins/terminal/init.lua',
  install_dir: get_option('data_dir') / 'plugins' / 'terminal'
)
