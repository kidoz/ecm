project(
  'ecm',
  'c',
  version: '1.0.0',
  license: 'GPL-2.0-or-later',
  default_options: [
    'c_std=c23',
    'warning_level=2',
    'buildtype=release',
  ],
)

inc = include_directories('include')

ecm_exe = executable(
  'ecm',
  'src/ecm.c',
  include_directories: inc,
  install: true,
)

unecm_exe = executable(
  'unecm',
  'src/unecm.c',
  include_directories: inc,
  install: true,
)
