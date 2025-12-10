project(
  'ecm',
  'c',
  version: '1.0.0',
  license: 'GPL-2.0-or-later',
  default_options: [
    'c_std=c23',
    'warning_level=2',
    'werror=true',
    'buildtype=release',
  ],
)

inc = include_directories('include')

# Shared ECC/EDC library
eccedc_lib = static_library(
  'eccedc',
  'src/eccedc.c',
  include_directories: inc,
)

ecm_exe = executable(
  'ecm',
  'src/ecm.c',
  include_directories: inc,
  link_with: eccedc_lib,
  install: true,
)

unecm_exe = executable(
  'unecm',
  'src/unecm.c',
  include_directories: inc,
  link_with: eccedc_lib,
  install: true,
)

# Tests
subdir('tests')
