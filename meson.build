project(
  'ecm',
  'c',
  version: '1.2.0',
  license: 'GPL-2.0-or-later',
  default_options: [
    'c_std=c23',
    'warning_level=2',
    'werror=true',
    'buildtype=release',
  ],
)

# Generate version header
version_h = configure_file(
  input: 'include/version.h.in',
  output: 'version.h',
  configuration: {'VERSION': meson.project_version()},
)

inc = include_directories('include')
inc_build = include_directories('.')

# Thread library dependency (pthread on POSIX systems without C11 threads)
thread_dep = dependency('threads', required: false)

# Shared ECC/EDC library
eccedc_lib = static_library(
  'eccedc',
  'src/eccedc.c',
  include_directories: inc,
  dependencies: thread_dep,
)

ecm_exe = executable(
  'ecm',
  'src/ecm.c',
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
  install: true,
)

unecm_exe = executable(
  'unecm',
  'src/unecm.c',
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
  install: true,
)

# Tests
subdir('tests')
