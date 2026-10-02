# ECM

**Error Code Modeler** - Lossless compression for CD image files

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![C Standard](https://img.shields.io/badge/C-C23-blue.svg)](https://en.cppreference.com/w/c/23)
[![Build System](https://img.shields.io/badge/Build-Meson-green.svg)](https://mesonbuild.com/)

## Overview

ECM reduces the size of CD image files (BIN, CDI, NRG, CCD, and other raw sector formats) by eliminating redundant Error Correction/Detection Codes (ECC/EDC) from each sector.

The encoder automatically detects sector types and strips predictable data. The decoder regenerates every sector exactly as it was stored: full 2352-byte raw sectors, or 2336-byte Mode 2 sectors for images that never had sync and header bytes. Compression is **completely lossless**, typically shrinking game discs by 10-15%.

> **Note:** "Cooked" ISO files (2048 bytes/sector) contain no ECC/EDC data and will see no size reduction.

## Quick start

```bash
ecm game.bin          # game.bin -> game.bin.ecm
unecm game.bin.ecm    # game.bin.ecm -> game.bin
```

## Documentation

The documentation is organized by what you need, following [Diátaxis](https://diataxis.fr/):

**Tutorials** — learning by doing
- [Compress and restore your first CD image](doc/tutorials/getting-started.md)

**How-to guides** — steps for a specific task
- [Install ECM](doc/how-to/install.md)
- [Compress CD images](doc/how-to/compress-images.md)
- [Restore CD images](doc/how-to/restore-images.md) — including CUE sheets
- [Stream images through pipes](doc/how-to/stream-through-pipes.md)
- [Decode archives from old ECM versions](doc/how-to/decode-old-archives.md)

**Reference** — precise, complete information
- [Command-line reference](doc/reference/cli.md)
- [ECM file format specification](doc/reference/format.md)

**Explanation** — background and design
- [How ECM works](doc/explanation/how-ecm-works.md)
- [Compatibility](doc/explanation/compatibility.md)

## Features

- **Lossless** - Bit-perfect roundtrip compression/decompression
- **Automatic detection** - Identifies Mode 1 and Mode 2 Form 1/2 sectors at any byte offset, including MODE2/2336 images and raw dumps with subchannel data
- **Streaming support** - Works with stdin/stdout for pipeline integration
- **CUE generation** - Optional CUE sheet creation on decode
- **Cross-platform** - Linux, macOS, Windows

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Ensure tests pass (`meson test -C build`, or `just test`)
4. Ensure code is formatted (`just fmt`)
5. Submit a pull request

Build requirements and commands are in [Install ECM](doc/how-to/install.md).

## License

This project is licensed under the **GNU General Public License v2.0** - see the [LICENSE](LICENSE) file for details.

## Authors

- Aleksandr Pavlov <ckidoz@gmail.com>

See [AUTHORS.md](AUTHORS.md) for full attribution.

## See Also

- [ECM Tools](https://web.archive.org/web/20150419125413/http://www.neillcorlett.com/ecm/) - Original implementation by Neill Corlett
- [cdrdao](https://cdrdao.sourceforge.net/) - CD burning with DAO support
