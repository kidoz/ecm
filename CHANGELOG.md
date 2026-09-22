# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added

- **`unecm --mode2-2352`** - Expands Mode 2 records to 2352-byte sectors with regenerated
  sync, address and mode, the way versions 1.2.0 to 1.3.1 decoded them. Archives those
  versions made from raw Mode 2 images decode back to the original image again, provided
  the addresses were sequential from 00:02:00, which is all those versions could reproduce.
  Options to `unecm` may now be given in any order; an unknown option is an error

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
