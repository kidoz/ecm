# ECM

**Error Code Modeler** - Lossless compression for CD image files

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![C Standard](https://img.shields.io/badge/C-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Build System](https://img.shields.io/badge/Build-Meson-green.svg)](https://mesonbuild.com/)

## Overview

ECM reduces the size of CD image files (BIN, CDI, NRG, CCD, and other raw sector formats) by eliminating redundant Error Correction/Detection Codes (ECC/EDC) from each sector.

The encoder automatically detects sector types and strips predictable data. The decoder regenerates full 2352-byte raw sectors. Compression is **completely lossless**.

> **Note:** "Cooked" ISO files (2048 bytes/sector) contain no ECC/EDC data and will see no size reduction.

## Features

- **Lossless** - Bit-perfect roundtrip compression/decompression
- **Automatic detection** - Identifies Mode 1, Mode 2 Form 1/2 sectors
- **Streaming support** - Works with stdin/stdout for pipeline integration
- **CUE generation** - Optional CUE sheet creation on decode
- **Cross-platform** - Linux, macOS, Windows

## Installation

### From Source

#### Requirements

- C23 compatible compiler (GCC 14+, Clang 18+)
- [Meson](https://mesonbuild.com/) build system
- [Ninja](https://ninja-build.org/) (recommended)

#### Build

```bash
# Configure and build
meson setup build
meson compile -C build

# Run tests
meson test -C build

# Install (optional)
sudo meson install -C build
```

Or using the [`just`](https://github.com/casey/just) command runner:

```bash
just build      # Build release
just rebuild    # Clean and rebuild
just test       # Run tests
just benchmark  # Run performance benchmarks
```

### Homebrew (macOS)

```bash
brew install --cask ecm
```

## Usage

### Encoding

Compress a CD image by stripping ECC/EDC data:

```bash
ecm game.bin                    # Creates game.bin.ecm
ecm game.bin compressed.ecm     # Custom output name
cat game.bin | ecm - - > out.ecm  # Streaming mode
```

**Full syntax:**

```bash
ecm [-v|--verbose] <input> [output]
```

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Show sector processing details |
| `input` | CD image file (BIN, CDI, NRG, CCD, etc.) or `-` for stdin |
| `output` | ECM file (defaults to `<input>.ecm`) or `-` for stdout |

### Decoding

Restore the original CD image:

```bash
unecm game.bin.ecm              # Creates game.bin
unecm --cue game.bin.ecm        # Also creates game.bin.cue
unecm game.bin.ecm restored.bin # Custom output name
```

**Full syntax:**

```bash
unecm [-v|--verbose] [--cue] <input.ecm> [output]
```

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Show record decoding details |
| `--cue` | Generate a CUE sheet file |
| `input.ecm` | ECM file (must end in `.ecm`) or `-` for stdin |
| `output` | Output file (defaults to input without `.ecm`) or `-` for stdout |

## How It Works

CD-ROM sectors contain user data plus error correction codes. ECM identifies sector types and strips the predictable portions:

| Sector Type | Raw Size | Stored | Savings | Description |
|-------------|----------|--------|---------|-------------|
| Mode 1 | 2352 B | 2051 B | ~13% | Standard data with full ECC/EDC |
| Mode 2 Form 1 | 2352 B | 2052 B | ~13% | XA data with ECC/EDC |
| Mode 2 Form 2 | 2352 B | 2336 B | ~1% | XA audio/video (EDC only) |
| Literal | varies | varies | 0% | Non-standard data (stored as-is) |

Typical compression ratios for game discs: **10-15% size reduction**.

## Processing Modes

ECM automatically selects the optimal mode based on input type:

| Mode | Input Type | Buffer | Best For |
|------|-----------|--------|----------|
| **Batch** | Regular files | ~1 MB | Large files, best compression |
| **Streaming** | stdin/pipes | ~2.4 KB | Pipelines, memory-constrained |

Batch mode groups consecutive same-type sectors for better compression ratios. Use regular files when possible for best results.

## File Format

See [FORMAT.md](doc/FORMAT.md) for the ECM file format specification.

## Performance

Run benchmarks with:

```bash
just benchmark
```

Example results (Apple M1):

```
EDC Computation:     ~370 MB/s
ECC Generation:      ~220,000 sectors/sec
Sector Detection:    ~104,000 sectors/sec
Encode Throughput:   ~120 MB/s
Decode Throughput:   ~120 MB/s
```

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Ensure tests pass (`meson test -C build`)
4. Ensure code is formatted (`just fmt`)
5. Submit a pull request

## License

This project is licensed under the **GNU General Public License v2.0** - see the [LICENSE](LICENSE) file for details.

## Authors

- Aleksandr Pavlov <ckidoz@gmail.com>

See [AUTHORS.md](AUTHORS.md) for full attribution.

## See Also

- [ECM Tools](https://web.archive.org/web/20150419125413/http://www.neillcorlett.com/ecm/) - Original implementation by Neill Corlett
- [cdrdao](https://cdrdao.sourceforge.net/) - CD burning with DAO support
