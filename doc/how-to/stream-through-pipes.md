# Stream images through pipes

This guide shows how to use `ecm` and `unecm` in a pipeline, so an image can be
compressed or restored without a temporary file in the middle.

## Compress from standard input

Write the image to standard input and the archive to standard output with `-`:

```bash
cat game.bin | ecm - - > game.bin.ecm
```

`-` is a file name meaning standard input (the first one) or standard output (the
second). With `-` as the input, the output defaults to `-` as well, so this is
equivalent:

```bash
cat game.bin | ecm - > game.bin.ecm
```

## Restore to standard output

```bash
unecm - - < game.bin.ecm > restored.bin
```

Or pipe the archive in:

```bash
cat game.bin.ecm | unecm - - > restored.bin
```

As with the encoder, an input of `-` makes `-` the default output.

## Combine with other tools

The archive is a plain byte stream, so it composes with anything:

```bash
ecm - - < game.bin | zstd -o game.bin.ecm.zst
zstd -dc game.bin.ecm.zst | unecm - - > game.bin
```

This is handy for piping across a network:

```bash
ecm - - < game.bin | ssh host 'cat > game.bin.ecm'
```

## Keep in mind

- A pipe cannot be seeked, so `ecm` uses its streaming encoder instead of the batch
  encoder. Both are lossless and detect sectors the same way; batch mode groups runs of
  same-type sectors slightly better. See [How ECM works](../explanation/how-ecm-works.md).
- `--cue` writes its sheet next to the image, so it requires a file output and is
  rejected when the output is `-`.
- Use `--` before a file name that begins with `-`, and remember that a lone `-` is a
  file name, not an option.
