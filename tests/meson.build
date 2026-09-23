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

# Integration tests (roundtrip). Invoke bash explicitly with forward-slash paths: on
# Windows, find_program() would hand bash a backslashed script path it cannot open.
#
# bash is optional, so a machine without it can still build and run the unit tests. On
# Windows the bash.exe in System32 is the WSL launcher, which cannot run these Windows
# binaries, and Git's usr\bin\bash.exe only works inside a Git Bash session. Prefer Git for
# Windows' bin\bash.exe wrapper, which sets up its own environment: git.exe lives in
# <git>\cmd, or in <git>\mingw64\bin inside Git Bash.
fs = import('fs')
bash = disabler()
if host_machine.system() == 'windows'
  git = find_program('git', required: false)
  if git.found()
    git_dir = fs.parent(git.full_path())
    foreach root : [fs.parent(git_dir), fs.parent(fs.parent(git_dir))]
      if not bash.found() and fs.is_file(root / 'bin' / 'bash.exe')
        bash = find_program(root / 'bin' / 'bash.exe')
      endif
    endforeach
  endif
endif
if not bash.found()
  candidate = find_program('bash', required: false)
  if candidate.found() and not candidate.full_path().to_lower().contains('system32')
    bash = candidate
  endif
endif

if bash.found()
  test(
    'integration_roundtrip',
    bash,
    args: [
      fs.as_posix(meson.current_source_dir() / 'roundtrip_test.sh'),
      fs.as_posix(ecm_exe.full_path()),
      fs.as_posix(unecm_exe.full_path()),
    ],
    depends: [ecm_exe, unecm_exe],
  )
else
  message('bash not found (WSL\'s launcher does not count): integration_roundtrip is skipped')
endif
