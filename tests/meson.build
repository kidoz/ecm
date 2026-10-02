# Unit tests (include-based, access static functions)
# Link against the shared eccedc library to avoid duplicate symbols
test_ecm_exe = executable(
  'test_ecm',
  'test_ecm.c',
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
)

test_unecm_exe = executable(
  'test_unecm',
  'test_unecm.c',
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
)

# Performance benchmarks
benchmark_exe = executable(
  'ecm_benchmark',
  'benchmark.c',
  include_directories: [inc, inc_build],
  link_with: eccedc_lib,
)

# Register unit tests with Meson
test('unit_ecm', test_ecm_exe)
test('unit_unecm', test_unecm_exe)

# Register benchmark (use 'meson test -C build perf_benchmark' or 'just benchmark')
test('perf_benchmark', benchmark_exe, is_parallel: false, timeout: 120)

# Integration tests (roundtrip). Python 3 builds the sector fixtures and drives the
# binaries, so no bash is needed and the suite runs natively on Windows too. Python is
# optional: without an interpreter the unit tests still build and run.
fs = import('fs')
python = import('python').find_installation(required: false)

if python.found()
  test(
    'integration_roundtrip',
    python,
    args: [
      fs.as_posix(meson.current_source_dir() / 'roundtrip_test.py'),
      fs.as_posix(ecm_exe.full_path()),
      fs.as_posix(unecm_exe.full_path()),
    ],
    depends: [ecm_exe, unecm_exe],
  )
else
  message('python3 not found: integration_roundtrip is skipped')
endif
