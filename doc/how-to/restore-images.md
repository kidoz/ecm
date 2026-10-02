# Restore CD images

This guide shows how to decode an `.ecm` archive back into its original image with
`unecm`, and how to write a CUE sheet alongside it.

## Restore an image

```bash
unecm game.bin.ecm
```

With one file name, `unecm` removes the `.ecm` suffix and writes `game.bin`. If a file
with that name already exists it is overwritten without prompting, so name an output
explicitly when you want to keep the original around:

```bash
unecm game.bin.ecm restored.bin
```

A successful run looks like this:

```console
$ unecm game.bin.ecm restored.bin
UNECM - Decoder for Error Code Modeler format v1.3.4

Decoding game.bin.ecm to restored.bin.
Decoded 9126966 bytes -> 10466400 bytes
Done; file is OK
```

`Done; file is OK` means the whole-file EDC written by the encoder checked out, so the
restored image matches the image that was compressed.

## Write a CUE sheet

Add `--cue` to write a single-track CUE sheet next to the image:

```console
$ unecm --cue game.bin.ecm
UNECM - Decoder for Error Code Modeler format v1.3.4

Decoding game.bin.ecm to game.bin.
Decoded 9126966 bytes -> 10466400 bytes
Created CUE file: game.bin.cue
Done; file is OK
```

The sheet is written as `<output>.cue` and names the image by file name alone, because
players resolve the `FILE` entry relative to the sheet:

```
FILE "game.bin" BINARY
  TRACK 01 MODE1/2352
    INDEX 01 00:00:00
```

The track mode follows what was decoded: `MODE1/2352`, `MODE2/2352`, or `MODE2/2336` for
images made of header-less Mode 2 bodies. An image that mixes 2336-byte Mode 2 bodies
with 2352-byte sectors has no single track mode; `unecm` writes the sheet and warns.

`--cue` needs a file output, so it cannot be combined with `-` (stdout). The image name
must not contain a quote or a newline, which a CUE `FILE` line cannot hold. The sheet
path gets the same safety checks as the image: it is refused if it is a symlink or names
the same file as the archive or the image.

## Archives from old versions

Archives written by `ecm` 1.2.0 to 1.3.1 from raw Mode 2 images need an extra flag to
decode correctly; see [Decode archives from old ECM versions](decode-old-archives.md).

## If a decode fails

On a corrupt or truncated archive, `unecm` reports the problem on standard error
(`Corrupt ECM file!`, `EDC error (...)`, or `Unexpected EOF!`) and does not leave a
partial image behind: the incomplete output file is emptied and removed.

## Where to go next

- [Stream images through pipes](stream-through-pipes.md) to decode without a file.
- [Command-line reference](../reference/cli.md) for every option and file-handling rule.
