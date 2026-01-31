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

# Integration tests (roundtrip)
roundtrip_test = find_program('roundtrip_test.sh')
test(
  'integration_roundtrip',
  roundtrip_test,
  args: [ecm_exe.full_path(), unecm_exe.full_path()],
  depends: [ecm_exe, unecm_exe],
)
