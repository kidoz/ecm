# Command-line reference

Reference for the `ecm` encoder and the `unecm` decoder: syntax, options, operands, and
the file-handling rules both tools follow.

## ecm

### Synopsis

```
ecm [-v|--verbose] cdimagefile [ecmfile]
ecm -h|--help | -V|--version
```

### Options

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Report processing details: the mode chosen, each record written, and a summary of sector counts and sizes |
| `-h`, `--help` | Print usage and exit |
| `-V`, `--version` | Print `ecm <version>` and exit |

### Operands

| Operand | Description |
|---------|-------------|
| `cdimagefile` | CD image (BIN, CDI, NRG, CCD, or any raw sector dump), or `-` for standard input |
| `ecmfile` | Output archive, or `-` for standard output. Defaults to `cdimagefile` with `.ecm` appended, or to `-` when the input is `-` |

## unecm

### Synopsis

```
unecm [-v|--verbose] [--cue] [--mode2-2352] ecmfile [outputfile]
unecm -h|--help | -V|--version
```

### Options

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Report each record as it is decoded |
| `--cue` | Write a single-track CUE sheet as `<output>.cue`; see [CUE sheets](#cue-sheets) |
| `--mode2-2352` | Expand Mode 2 records to 2352-byte sectors with regenerated sync, address, and mode, as versions 1.2.0 to 1.3.1 did. Use it only for archives those versions made from raw Mode 2 images; see [Decode archives from old ECM versions](../how-to/decode-old-archives.md) |
| `-h`, `--help` | Print usage and exit |
| `-V`, `--version` | Print `unecm <version>` and exit |

### Operands

| Operand | Description |
|---------|-------------|
| `ecmfile` | ECM archive, or `-` for standard input. A file name must end in `.ecm` |
| `outputfile` | Output image, or `-` for standard output. Defaults to `ecmfile` with the `.ecm` suffix removed, or to `-` when the input is `-` |

## Option and file-name rules

Both tools follow the same rules:

- Options may appear before or after file names.
- `--` ends option processing; use it before a file name that starts with `-`.
- A lone `-` is a file name (standard input or output), not an option.
- An unknown option is an error, not a file name.
- At most two file names are accepted; a third is an error.
- With no file names, the usage message is printed and the exit status is 1.

## Exit status

`0` on success, `1` on any error. The success lines — `Done.` for `ecm` and
`Done; file is OK` for `unecm` — are printed only after the output has been flushed, so
their absence means the run did not complete.

## File handling

- **Output naming.** Defaults are described in the operand tables above. An output path
  that is given explicitly is used as-is.
- **Overwrite.** An existing output file is replaced without prompting.
- **Aliasing and symlinks.** Before an output is opened (the image, and with `--cue`
  also the sheet), a path that is a symlink, or that names the same file as an input, is
  refused. Opening the output would truncate it, so the check happens first: the input
  archive and a symlink's target are never destroyed by a run that could not proceed.
- **Failure cleanup.** When a run fails, the incomplete output file is emptied and then
  removed, so nothing keeps the partial bytes — not even another hard link to the same
  file. A failure to remove is reported on standard error. With `--cue`, no sheet is
  written unless the image decoded successfully.
- **Verification.** `unecm` verifies the decoded stream against the whole-file EDC stored
  in the archive and reports `EDC error (...)`, `Corrupt ECM file!`, or
  `Unexpected EOF!` on a damaged archive.

## CUE sheets

`unecm --cue` writes a single-track sheet as `<output>.cue`:

```
FILE "game.bin" BINARY
  TRACK 01 MODE1/2352
    INDEX 01 00:00:00
```

The `FILE` entry names the image by file name alone, because players resolve it relative
to the sheet. The track mode follows the decoded sector size:

| Decoded image | Track mode |
|---------------|------------|
| Mode 1 sectors | `MODE1/2352` |
| Raw Mode 2 sectors | `MODE2/2352` |
| Only header-less 2336-byte Mode 2 bodies | `MODE2/2336` |

An image that mixes 2336-byte Mode 2 bodies with 2352-byte sectors has no single track
mode; `unecm` writes the sheet and warns.

`--cue` requires a file output: combining it with `-` (standard output) is an error. The
output file name must not contain a quote or a newline, which a CUE `FILE` line cannot
hold. The sheet path is refused if it is a symlink or names the same file as the input
archive or the output image.
