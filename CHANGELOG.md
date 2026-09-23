# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [1.3.4] - 2026-09-23

### Fixed

- **Record counts beyond the format limit** - The encoder splits runs so that no record count
  reaches 2^31, which the format declares invalid. Runs used to be split only near 2^32, so
  inputs with more than 2 GiB of consecutive literal data produced files that a decoder
  enforcing the limit rejects. `unecm` still reads such files
- **Sectors off the 2352-byte grid** - Both encoders look for sectors at every byte offset
  again, as 1.1.0 and the original `ecm` did, and recognise header-less 2336-byte Mode 2
  bodies. Since 1.3.0 only multiples of 2352 from the start of the input were tested, so one
  leading byte disabled all compression and MODE2/2336 images or dumps with subchannel data
  were stored as literal bytes. Window EDCs are rolled forward byte by byte, so repetitive
  padding does not slow the scan down
- **Last sector at a buffer boundary** - A sector straddling the end of the 1 MB analysis
  buffer was stored as literal bytes when most of it was already buffered, which affected the
  final sector of images whose sector count is one more than a multiple of 445
- **CUE sheet file reference** - `unecm --cue` names the image in the sheet's `FILE` entry by
  its file name alone. It used to copy the output path as typed, but the sheet is written next
  to the image and players resolve the entry relative to the sheet, so decoding to
  `outdir/restored.bin` produced a sheet pointing at `outdir/outdir/restored.bin`
- **CUE sheet track mode** - The track mode follows the decoded sector size. Mode 2 records
  without a literal header in front decode to 2336-byte sectors, and the sheet now says
  MODE2/2336 for them instead of MODE2/2352
- **Unicode file names on Windows** - The executables embed a manifest that makes UTF-8 their
  code page, so file names outside the legacy ANSI code page no longer reach the C runtime as
  `?` and fail to open. Needs Windows 10 version 1903 or later
- **Misplaced options** - Both tools recognise options anywhere on the command line, and `--`
  ends them. An option after the file names used to become the output file name, so
  `unecm game.bin.ecm --cue` wrote a file called `--cue`. Unknown options and extra file
  names are errors
- **Incomplete output left behind** - When encoding or decoding fails, the output file is
  deleted instead of remaining under the requested name, where a truncated or unverified
  image could pass for a finished one
- **Reports** - Byte totals no longer come from the position of a pipe, which reported zero
  or garbage when piping. Statistics counters are 64-bit, so they no longer wrap above 4 GiB,
  and the progress line only appears when stderr is a terminal

### Added

- **`-h`/`--help` and `-V`/`--version`** for both tools

### Changed

- **Warnings are no longer errors by default** - A plain `meson setup` builds with
  `werror=false`, so a new warning from a newer compiler cannot break a distribution build.
  The `just` recipes pass `-Dwerror=true`
- **Meson 1.3 or newer** is required. `c_std` falls back to `c2x` for compilers that
  implement the C23 features used here under the older name

### Development

- **Integration test on Windows** - Meson finds Git for Windows' bash next to `git.exe` and
  never picks the WSL launcher in System32, which cannot run the Windows binaries. Without a
  usable bash the test is skipped instead of failing the setup. The script falls back to
  `python` when `python3` is missing, and it now covers off-grid sectors, CUE track modes,
  option parsing, and removal of failed output
- **`just`** - New `test` recipe. The Windows recipes use `build-msvc` and `build-clang` so a
  directory configured for another compiler is never reused. `clean`, `rebuild` and `wipe`
  refuse to delete a build directory holding disc images or archives
- **Arch Linux packaging** - `checkdepends` lists Python, which the integration test in
  `check()` needs in a clean chroot

## [1.3.3] - 2026-09-22

### Added

- **`unecm --mode2-2352`** - Expands Mode 2 records to 2352-byte sectors with regenerated
  sync, address and mode, the way versions 1.2.0 to 1.3.1 decoded them. Archives those
  versions made from raw Mode 2 images decode back to the original image again, provided
  the addresses were sequential from 00:02:00, which is all those versions could reproduce.
  Options to `unecm` may now be given in any order; an unknown option is an error

### Development

- **Windows build recipes** - Meson native files under `native/` configure static-CRT release
  builds with the Visual Studio compiler (`windows-msvc.ini`, which supplies the `/std:clatest`
  and `nullptr` workarounds MSVC needs) or clang (`windows-clang.ini`), with matching
  `just build-windows` and `just build-windows-clang` recipes and a README section

## [1.3.2] - 2026-09-22

### Fixed

- **Mode 2 addresses lost on roundtrip** - Raw Mode 2 sectors are encoded as a 16-byte literal
  record (sync, address, mode) followed by the type 2/3 record, the layout the original `ecm`
  produces. The decoder no longer invents sequential addresses, so `unecm(ecm(x)) == x` holds
  for Mode 2 images whose addresses do not start at 00:02:00
- **Mode 2 record size** - Type 2/3 records expand to the 2336-byte body the specification
  defines. Streams written by the original `ecm` now decode correctly instead of growing by
  16 bytes per sector while still passing the EDC check
- **Input destroyed when the output path aliased it** - Both tools refuse to run when the
  output names the input file, including through symlinks and hard links, instead of
  truncating it and reporting success
- **Silent output failures** - Both tools check the final flush/close of the output (and
  `unecm --cue` of the CUE sheet) and exit non-zero when buffered data could not be written
- **Read errors treated as end of input** - Streaming mode (`ecm -`) reports a failing read
  instead of writing a valid empty archive and exiting 0
- **Streaming mode on Windows** - `ecm - -` and `unecm - -` switch stdin/stdout to binary
  mode. The C runtime's text mode stopped reading at the first 0x1A byte and turned every
  0x0A written into 0x0D 0x0A, so piped images were truncated and corrupted
- **MinGW-w64 GCC build** - `<threads.h>` is probed with `__has_include` and the POSIX
  `pthread_once` fallback is used when it is missing; the 64-bit seek macros no longer clash
  with the ones MinGW's `<stdio.h>` already defines

### Changed

- Files produced by versions 1.2.0 to 1.3.1 from raw Mode 2 images now decode to 2336-byte
  sectors, as the original `unecm` would, rather than to 2352-byte sectors with generated
  headers (see `doc/FORMAT.md`, "Files From Versions 1.2.0 to 1.3.1")
- **Packaging** - PKGBUILD and Homebrew formula point at the 1.3.1 release; the README
  installs the formula from this repository with Homebrew developer mode instead of a
  non-existent cask

### Development

- The test suite runs on Windows: the flush-failure test writes into a closed pipe instead of
  closing a descriptor under a live stream (fatal on the Windows CRT), the read-error test
  reads a write-only stream instead of a directory, the roundtrip script converts its temp
  directory for native Python and skips the symlink check when `ln -s` cannot create one,
  and Meson runs the script through bash with forward-slash paths
- New `stream_set_binary()` unit test
- New `tests/test_tmpfile.h`: scratch streams come from the temp directory instead of
  `tmpfile()`, which on the Windows CRTs targets the drive root and fails without
  administrator rights

## [1.3.1] - 2026-04-08

### Fixed

- **Windows build** - Fixed compilation issues on Windows
- **64-bit types on Windows** - Fixed problem with 64-bit types on Windows

### Changed

- **Packaging** - Updated packaging configuration

## [1.3.0] - 2026-01-31

### Added

- **Verbose mode** - New `-v`/`--verbose` flag for both `ecm` and `unecm`
  - `ecm -v` shows mode selection and batch flush events
  - `unecm -v` shows record type/count decoding
- **Debug logging** - Compile-time `ECM_DEBUG` macro for development builds
  - `ECM_DEBUG_LOG()` macro outputs to stderr with `[DEBUG]` prefix
  - `ECM_VERBOSE()` macro for runtime-controlled verbose output
- **Performance benchmarks** - New benchmark suite (`just benchmark`)
  - EDC computation throughput (~370 MB/s)
  - ECC generation speed (~220K sectors/sec)
  - Sector type detection (~104K sectors/sec)
  - Full encode/decode throughput (~120 MB/s)
- **Processing modes documentation** - README section explaining batch vs streaming trade-offs

### Changed

- **README overhaul** - Comprehensive documentation update
  - Added "Processing Modes" section
  - Added "Performance" section with benchmark results
  - Added "Contributing" guidelines
  - Added "See Also" section with related projects
  - Improved usage examples and option tables
- **Code comments** - Added detailed comments explaining:
  - Why `check_type_raw()` parameter cannot be `const` (calls `ecc_verify` with buffer modification)
  - Trade-offs between batch and streaming encoding modes

### Development

- New `tests/benchmark.c` performance measurement suite
- New `just benchmark` command
- Updated `tests/meson.build` to register benchmark executable

## [1.2.0] - 2025-12-12

### Fixed

- **Mode 2 MSF reconstruction** - Fixed incorrect MSF (Minute:Second:Frame) addresses when decoding Mode 2 sectors
  - Sector addresses were wrong when literal bytes preceded Mode 2 sectors
  - Now correctly tracks output position for MSF computation
- **Pipe/stdout support** - Fixed Mode 2 decoding to stdout/pipes
  - Previously `ftello()` returned -1 on non-seekable outputs, causing all sectors to get MSF 00:02:00
  - Now uses explicit byte counting instead of file position queries

### Changed

- **C23 modernization** - Adopted C23 `nullptr` keyword for null pointer constants
- **Code style** - Applied consistent formatting with clang-format
- **Source headers** - Removed redundant license/copyright headers from individual source files (see LICENSE file)

### Added

- **clang-format configuration** - Added `.clang-format` for consistent code style
- **stdin/stdout documentation** - Documented `-` parameter for stdin/stdout streaming

## [1.1.0] - 2025-12-10

### Added

- **Meson build system** - Modern build system replacing manual compilation
  - Release and debug build configurations
  - Automatic dependency detection
  - Cross-platform support
- **Test suite** - Comprehensive testing infrastructure
  - Unit tests for ECM encoder (`test_ecm.c`)
  - Unit tests for UNECM decoder (`test_unecm.c`)
  - Integration roundtrip tests (`roundtrip_test.sh`)
- **Shared library** - Extracted common ECC/EDC code into `libeccedc`
  - `eccedc.h` - Public API with sector constants and functions
  - `eccedc.c` - Shared implementation for both encoder and decoder
- **Arch Linux packaging** - PKGBUILD for easy installation on Arch-based systems
- **`just` command runner** - Convenient build commands (`just build`, `just rebuild`, `just clean`)

### Changed

- **C23 standard** - Updated to modern C23 language standard
- **Large file support** - Progress tracking now uses `int64_t` for files >4GB
- **Error handling** - Improved error propagation and reporting
  - All `fwrite()` calls now check return values
  - `ecmify()` properly returns error codes on failure
  - Added bounds checking in type/count decoder to prevent overflow
- **Code quality** - Compiler warnings treated as errors (`werror=true`)
- **Documentation** - Migrated to Markdown format
  - Modern GitHub-style README with badges and tables
  - FORMAT.md specification document

### Fixed

- **Integer overflow** - Fixed potential overflow in progress tracking for large files
- **Buffer overflow** - Added bounds check in ECM type/count decoding to reject malformed files
- **Write errors** - Now properly detected and reported (disk full, I/O errors)
- **Error propagation** - Encoding errors now correctly return non-zero exit code

### Removed

- **`include/unecm.h`** - Merged into `include/eccedc.h`
- **Win32 EXE files** - Removed pre-built binaries (build from source instead)

## [1.0.0] - 2002-xx-xx

### Added

- Initial release by Neill Corlett
- ECM encoder for CD image compression
- UNECM decoder for CD image restoration
- Support for Mode 1 and Mode 2 (Form 1/Form 2) sectors
- CUE file generation option

[1.3.1]: https://github.com/kidoz/ecm/compare/v1.3.0...v1.3.1
[1.3.0]: https://github.com/kidoz/ecm/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/kidoz/ecm/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/kidoz/ecm/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/kidoz/ecm/releases/tag/v1.0.0
