project(
  'ecm',
  'c',
  version: '1.3.3',
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

if host_machine.system() == 'windows'
  add_project_arguments('-D_CRT_SECURE_NO_WARNINGS', language: 'c')
endif

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

# Windows: embed a manifest that makes UTF-8 the process code page (Windows 10 1903 and
# later). The tools use the narrow C runtime API, so without it a file name outside the legacy
# ANSI code page arrives as '?' and cannot be opened.
win_resources = []
if host_machine.system() == 'windows'
  windows = import('windows')
  win_resources = windows.compile_resources(
    'src/windows/utf8.rc',
    depend_files: files('src/windows/utf8.manifest'),
    include_directories: include_directories('src/windows'),
  )
endif

ecm_exe = executable(
  'ecm',
  'src/ecm.c',
  win_resources,
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
  install: true,
)

unecm_exe = executable(
  'unecm',
  'src/unecm.c',
  win_resources,
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
  install: true,
)

# Tests
subdir('tests')
