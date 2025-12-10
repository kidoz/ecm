# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[1.1.0]: https://github.com/kidoz/ecm/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/kidoz/ecm/releases/tag/v1.0.0
