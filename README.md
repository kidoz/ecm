# ECM

**Error Code Modeler** - Encode/decode utilities for CD image compression

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![C Standard](https://img.shields.io/badge/C-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Build System](https://img.shields.io/badge/Build-Meson-green.svg)](https://mesonbuild.com/)

## Overview

ECM reduces the size of CD image files (BIN, CDI, NRG, CCD, and other raw sector formats) by eliminating redundant Error Correction/Detection Codes (ECC/EDC) from each sector.

The encoder automatically detects sector types and skips headers. Results vary depending on redundant ECC/EDC data present. Note: "cooked" ISO files will see no reduction.

## Features

- Supports Mode 1 and Mode 2 (Form 1/Form 2) CD sectors
- Automatic sector type detection
- Lossless compression/decompression
- Optional CUE file generation
- Cross-platform (Linux, Windows, macOS)

## Building

### Requirements

- C23 compatible compiler (GCC 14+, Clang 18+)
- [Meson](https://mesonbuild.com/) build system
- [Ninja](https://ninja-build.org/) (recommended)

### Build Instructions

```bash
# Configure and build
meson setup build
meson compile -C build

# Run tests
meson test -C build

# Install (optional)
meson install -C build
```

Or using the `just` command runner:

```bash
just build      # Build release
just rebuild    # Clean and rebuild
just clean      # Remove build directory
```

## Usage

### Encoding (ECM)

```bash
ecm <cdimagefile> [ecmfile]
```

| Argument | Description |
|----------|-------------|
| `cdimagefile` | Input CD image file (BIN, CDI, NRG, CCD, etc.), or `-` for stdin |
| `ecmfile` | Output ECM file (optional, defaults to `<input>.ecm`), or `-` for stdout |

**Example:**

```bash
ecm game.bin                  # Creates game.bin.ecm
ecm game.bin compressed.ecm   # Creates compressed.ecm
```

### Decoding (UNECM)

```bash
unecm [--cue] <ecmfile> [outputfile]
```

| Argument | Description |
|----------|-------------|
| `--cue` | Generate a CUE sheet file |
| `ecmfile` | Input ECM file (must end in `.ecm`), or `-` for stdin |
| `outputfile` | Output file (optional, defaults to input without `.ecm`), or `-` for stdout |

**Example:**

```bash
unecm game.bin.ecm              # Creates game.bin
unecm --cue game.bin.ecm        # Creates game.bin and game.cue
unecm game.bin.ecm restored.bin # Creates restored.bin
```

## How It Works

ECM analyzes CD image sectors and identifies patterns in ECC/EDC data:

| Sector Type | Size | Description |
|-------------|------|-------------|
| Mode 1 | 2352 bytes | Standard data sectors with full ECC/EDC |
| Mode 2 Form 1 | 2336 bytes | XA data sectors with ECC/EDC |
| Mode 2 Form 2 | 2336 bytes | XA audio/video sectors (EDC only) |
| Literal | variable | Non-standard data (stored as-is) |

The encoder strips predictable ECC/EDC bytes; the decoder regenerates them perfectly.

**Note:** Mode 1 sectors preserve original MSF addresses; Mode 2 sectors do not store MSF (per ECM format design). See [FORMAT.md](doc/FORMAT.md#limitations-and-notes) for details.

## File Format

See [FORMAT.md](doc/FORMAT.md) for detailed ECM file format specification.

## License

This project is licensed under the **GNU General Public License v2.0** - see the [LICENSE](LICENSE) file for details.

## Credits

- **Original Author:** Neill Corlett (2002)
- **Contributors:** See [GitHub contributors](../../graphs/contributors)
