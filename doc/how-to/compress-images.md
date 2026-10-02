# Compress CD images

This guide shows how to turn a CD image into a smaller `.ecm` archive with `ecm`.

## Compress an image

```bash
ecm game.bin
```

With one file name, `ecm` writes `<input>.ecm` — here `game.bin.ecm`.

## Choose the output name

```bash
ecm game.bin compressed.ecm
```

An existing output file is overwritten without prompting. Before anything is truncated,
`ecm` refuses an output path that is a symlink or that names the same file as the input;
see [File handling](../reference/cli.md#file-handling).

## What compresses

Raw sector images — BIN, CDI, NRG, CCD — shrink by roughly 10–15%. "Cooked" `.iso`
files store 2048 bytes per sector and contain no ECC/EDC, so they see no reduction: they
are stored as literal bytes and still round-trip exactly.
[How ECM works](../explanation/how-ecm-works.md) explains why.

Images also do not need to be plain sector streams. A leading prefix, 2336-byte Mode 2
sectors, or extra per-sector data such as subchannel bytes all still compress.

## See what the encoder found

Every run ends with a summary of what was stored:

```console
$ ecm game.bin
ECM - Encoder for Error Code Modeler format v1.3.4

Encoding game.bin to game.bin.ecm.
Literal bytes...........          0
Mode 1 sectors..........       4450
Mode 2 form 1 sectors...          0
Mode 2 form 2 sectors...          0
Encoded 10466400 bytes -> 9126966 bytes
Done.
```

The tallies count input bytes for `Literal` and sectors for the three sector types; the
last line gives the input and output sizes.

With `--verbose`, `ecm` also reports the processing mode it chose and each record as it
is written:

```console
$ ecm -v game.bin
ECM - Encoder for Error Code Modeler format v1.3.4

Encoding game.bin to game.bin.ecm.
Using batch mode (seekable file)
Flushing final batch: type=mode1, count=4450
Literal bytes...........          0
Mode 1 sectors..........       4450
...
Encoded 10466400 bytes -> 9126966 bytes
Done.
```

## Where to go next

- [Restore CD images](restore-images.md) to decode the archive.
- [Stream images through pipes](stream-through-pipes.md) to compress without a file.
- [Command-line reference](../reference/cli.md) for every option.
