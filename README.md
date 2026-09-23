# ECM

**Error Code Modeler** - Lossless compression for CD image files

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![C Standard](https://img.shields.io/badge/C-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Build System](https://img.shields.io/badge/Build-Meson-green.svg)](https://mesonbuild.com/)

## Overview

ECM reduces the size of CD image files (BIN, CDI, NRG, CCD, and other raw sector formats) by eliminating redundant Error Correction/Detection Codes (ECC/EDC) from each sector.

The encoder automatically detects sector types and strips predictable data. The decoder regenerates every sector exactly as it was stored: full 2352-byte raw sectors, or 2336-byte Mode 2 sectors for images that never had sync and header bytes. Compression is **completely lossless**.

> **Note:** "Cooked" ISO files (2048 bytes/sector) contain no ECC/EDC data and will see no size reduction.

## Features

- **Lossless** - Bit-perfect roundtrip compression/decompression
- **Automatic detection** - Identifies Mode 1 and Mode 2 Form 1/2 sectors at any byte offset, including MODE2/2336 images and raw dumps with subchannel data
- **Streaming support** - Works with stdin/stdout for pipeline integration
- **CUE generation** - Optional CUE sheet creation on decode
- **Cross-platform** - Linux, macOS, Windows

## Installation

### From Source

#### Requirements

- C23 compatible compiler: GCC 14+, Clang 18+, or Visual Studio 2022+ through the native file described below
- [Meson](https://mesonbuild.com/) 1.3 or newer
- [Ninja](https://ninja-build.org/) (recommended)
- For the integration test only: bash and Python 3. Meson skips that test when bash is missing.

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

Warnings are not errors in a plain `meson setup`, so a newer compiler cannot break a
release build. Add `-Dwerror=true` when developing; the `just` recipes and CI do.

Or using the [`just`](https://github.com/casey/just) command runner:

```bash
just build      # Build release
just rebuild    # Clean and rebuild
just test       # Run tests
just benchmark  # Run performance benchmarks
```

`just clean`, `just rebuild` and `just wipe` refuse to delete a build directory that holds
disc images or archives, since those are not build output.

#### Windows

Native files under `native/` configure a static-CRT release build, so the resulting
`ecm.exe` and `unecm.exe` need no VC++ redistributable.

With the Visual Studio compiler, from a *Developer Command Prompt for VS* (or after
running `vcvars64.bat`):

```bat
meson setup build --native-file native/windows-msvc.ini
meson compile -C build
```

With clang (LLVM for Windows):

```bat
meson setup build --native-file native/windows-clang.ini
meson compile -C build
```

The equivalent `just` recipes are `just build-windows` (MSVC, in `build-msvc`) and
`just build-windows-clang` (in `build-clang`), with `test-windows` and `test-windows-clang`
to run the tests. MSVC has no `nullptr` keyword in C mode yet, so the MSVC native file selects
`/std:clatest` and maps `nullptr` to `NULL`.

The Windows executables embed a manifest that makes UTF-8 their code page, so file names in
any script work on Windows 10 version 1903 and later. The integration test needs Git for
Windows' bash; Meson finds it next to `git.exe` and never uses the WSL launcher.

### Homebrew (macOS)

From a checkout of this repository, enable Homebrew developer mode to install the local
formula:

```bash
HOMEBREW_DEVELOPER=1 brew install --formula ./packaging/homebrew/ecm.rb
```

### Arch Linux

```bash
cd packaging/archlinux
makepkg -si
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
ecm -h|--help | -V|--version
```

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Show sector processing details |
| `-h`, `--help` | Print usage and exit |
| `-V`, `--version` | Print the version and exit |
| `input` | CD image file (BIN, CDI, NRG, CCD, etc.) or `-` for stdin |
| `output` | ECM file (defaults to `<input>.ecm`) or `-` for stdout |

Options may appear before or after the file names. Put `--` before a file name that starts
with `-`. Unknown options are errors rather than file names.

### Decoding

Restore the original CD image:

```bash
unecm game.bin.ecm              # Creates game.bin
unecm --cue game.bin.ecm        # Also creates game.bin.cue
unecm game.bin.ecm restored.bin # Custom output name
unecm --mode2-2352 old.bin.ecm  # Archive made by ecm 1.2.0-1.3.1 from a raw Mode 2 image
```

**Full syntax:**

```bash
unecm [-v|--verbose] [--cue] [--mode2-2352] <input.ecm> [output]
unecm -h|--help | -V|--version
```

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Show record decoding details |
| `--cue` | Write a single-track CUE sheet as `<output>.cue`. It names the image by file name, and its track mode follows the decoded sector size: MODE1/2352, MODE2/2352, or MODE2/2336 |
| `-h`, `--help` | Print usage and exit |
| `-V`, `--version` | Print the version and exit |
| `--mode2-2352` | Expand Mode 2 records to 2352-byte sectors with regenerated sync, address and mode, as versions 1.2.0 to 1.3.1 did. Use it only for archives those versions made from raw Mode 2 images; see [doc/FORMAT.md](doc/FORMAT.md) |
| `input.ecm` | ECM file (must end in `.ecm`) or `-` for stdin |
| `output` | Output file (defaults to input without `.ecm`) or `-` for stdout |

Options may appear anywhere, as with `ecm`. When a run fails, for example on a corrupt or
truncated archive, either tool deletes the incomplete output file rather than leave a
damaged image under the requested name.

## How It Works

CD-ROM sectors contain user data plus error correction codes. ECM identifies sector types and strips the predictable portions:

| Sector Type | Raw Size | Stored | Savings | Description |
|-------------|----------|--------|---------|-------------|
| Mode 1 | 2352 B | 2051 B | ~13% | Standard data with full ECC/EDC |
| Mode 2 Form 1 | 2352 B | 2068 B | ~12% | XA data with ECC/EDC; 16-byte sync/header kept as literal bytes |
| Mode 2 Form 2 | 2352 B | 2344 B | <1% | XA audio/video (EDC only); 16-byte sync/header kept as literal bytes |
| Literal | varies | varies | 0% | Non-standard data (stored as-is) |

Typical compression ratios for game discs: **10-15% size reduction**.

## Processing Modes

ECM automatically selects the optimal mode based on input type:

| Mode | Input Type | Buffer | Best For |
|------|-----------|--------|----------|
| **Batch** | Regular files | ~1 MB | Large files, best compression |
| **Streaming** | stdin/pipes | ~256 KB | Pipelines |

Batch mode groups consecutive same-type sectors for better compression ratios. Use regular files when possible for best results.

Both modes look for sectors at every byte offset, as the original ECM tools did, so images with
a prefix, with 2336-byte Mode 2 sectors, or with subchannel data after each sector still
compress.

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
