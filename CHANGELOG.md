# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - 2025-01-31

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

[1.3.0]: https://github.com/kidoz/ecm/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/kidoz/ecm/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/kidoz/ecm/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/kidoz/ecm/releases/tag/v1.0.0
