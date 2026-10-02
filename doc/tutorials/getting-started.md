# Compress and restore your first CD image

This tutorial walks you through one complete roundtrip with ECM: compress a CD image,
restore it, and verify that not a single byte changed. It needs no prior knowledge of
ECM and takes a few minutes.

**What you need**

- A CD image to work on — any raw dump such as a `.bin`, `.cdi`, `.nrg`, or `.ccd` file.
  A small one keeps the steps quick.
- The `ecm` and `unecm` tools on your `PATH`. If they are not installed yet, follow
  [Install ECM](../how-to/install.md) first and come back.

**What you will learn**

- How to compress an image and where the output goes.
- How to restore it and read the verification message.
- How to prove that the restoration was lossless.

The example below uses `game.bin`, a 10 MB image made of standard Mode 1 sectors. Use any
image you have; the commands and messages are the same.

## 1. Look at the original

```console
$ ls -l game.bin
-rw-r--r-- 1 you you 10466400 Jan  1 12:00 game.bin
```

Note the size — 10,466,400 bytes here. You will compare against it in the last step.

## 2. Compress the image

Run `ecm` with the image as its only argument:

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

With no output name given, `ecm` appends `.ecm` to the input name. The report at the end
tells you what the encoder found: this image is 4450 Mode 1 sectors and no literal bytes.
Check the result:

```console
$ ls -l game.bin game.bin.ecm
-rw-r--r-- 1 you you 10466400 Jan  1 12:00 game.bin
-rw-r--r-- 1 you you  9126966 Jan  1 12:00 game.bin.ecm
```

The archive is about 13% smaller. The saving comes from the ECC/EDC bytes in every
sector, which `unecm` can regenerate, so storing them would be redundant.

> Some images do not shrink: a "cooked" `.iso` file holds 2048 bytes per sector and has
> no ECC/EDC to remove. That is expected — see
> [How ECM works](../explanation/how-ecm-works.md).

## 3. Restore the image

Restore to a **new** file so the original stays around to compare against:

```console
$ unecm game.bin.ecm restored.bin
UNECM - Decoder for Error Code Modeler format v1.3.4

Decoding game.bin.ecm to restored.bin.
Decoded 9126966 bytes -> 10466400 bytes
Done; file is OK
```

`Done; file is OK` means the whole-file checksum stored by `ecm` checked out. Without an
output name, `unecm` would strip `.ecm` and write to `game.bin`, overwriting the original
image — one more reason to name the output here.

## 4. Verify the roundtrip

Compare the restored file with the original, byte for byte:

```console
$ cmp game.bin restored.bin && echo identical
identical
```

If `cmp` is not available, compare hashes instead: `sha256sum game.bin restored.bin` on
Linux, `shasum -a 256 game.bin restored.bin` on macOS, or
`certutil -hashfile game.bin SHA256` on Windows. A changed file shows a different hash.

That is the whole point of ECM: compression is lossless, so the restored image is the
image you started with.

## Where to go next

- [Compress CD images](../how-to/compress-images.md) — output naming and verbose reports.
- [Restore CD images](../how-to/restore-images.md) — CUE sheets and failure behavior.
- [How ECM works](../explanation/how-ecm-works.md) — why dropping ECC/EDC is safe.
- [ECM file format](../reference/format.md) — the byte-level specification.
