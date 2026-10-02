# Decode archives from old ECM versions

This guide shows how to restore images from archives written by `ecm` versions 1.2.0 to
1.3.1 — the only archives that need the `--mode2-2352` option.

## The symptom

An archive made by `ecm` 1.2.0 to 1.3.1 from a raw Mode 2 image (2352-byte sectors with
sync and header) decodes to a MODE2/2336 image: every Mode 2 record expands to 2336
bytes, 16 bytes shorter than the sector that was compressed. Those versions dropped the
16-byte sync/address/mode header when encoding, so it cannot be restored from the
archive.

## The fix

Add `--mode2-2352`:

```bash
unecm --mode2-2352 old.bin.ecm restored.bin
```

Every Mode 2 record then expands to a 2352-byte sector whose sync, address, and mode
fields are regenerated, with the address counted from 00:02:00 across all output bytes,
literals included — the layout those versions produced when decoding. For an image with
sequential addresses (the common case for a single-track disc) this restores the raw
image byte for byte.

## Verify before you trust it

The option is never applied automatically: an archive those versions made from a raw
Mode 2 image is indistinguishable from a spec-conformant archive that describes a
MODE2/2336 image. If you still have a reference copy or a checksum of the original
image, compare against it:

```bash
sha256sum restored.bin
```

## When not to use it

- Archives written by this tool (1.3.2 or later) or by the original `ecm` already store
  the header bytes, so `--mode2-2352` would duplicate 16 bytes per Mode 2 sector.
- Archives that were already MODE2/2336 — no sync or header in the source image — should
  be decoded normally.

[Compatibility](../explanation/compatibility.md) gives the full version history and
explains why the two layouts cannot be told apart.
