# Compatibility

ECM archives are exchanged between tools and versions, so it matters which of them can
read which files. This page records what this implementation guarantees and what changed
along the way.

## Interoperability with the original tools

Streams written by this tool decode with the original `unecm`, and streams written by
the original `ecm` decode with this tool. The sections below cover the one exception:
archives from versions 1.2.0 to 1.3.1.

## Raw Mode 2 sectors and the 16-byte headers

A type 2 or type 3 record expands to the **2336-byte** Mode 2 body (flags through
EDC/ECC). It has no field for sync, address, or mode. When the encoder meets a raw
2352-byte Mode 2 sector it therefore emits two records, exactly as the original `ecm`
did:

| Record | Content |
|--------|---------|
| Type 0, count 16 | Sync pattern, MSF address, and mode byte, stored verbatim |
| Type 2 or 3, count 1 | FLAGS + DATA for the 2336-byte body |

The original address survives the roundtrip because it is stored rather than
regenerated, and the decoded image is byte-for-byte identical to the input.

## Sector detection across versions

Like the original `ecm`, the encoder looks for a sector at every byte offset. Versions
1.3.0 to 1.3.3, however, only tested offsets that were multiples of 2352 from the start
of the input: one leading byte disabled all compression, and MODE2/2336 images or dumps
with subchannel data were stored as literal bytes. Files those versions produced still
decode normally; the same inputs simply compress better now.

## Record counts

The format declares counts of 2^31 and higher invalid. The encoder never writes such a
count: runs are split so that every record stays within the limit. Versions 1.3.1 to
1.3.3 only split runs near 2^32, so their archives of inputs with more than 2 GiB of
consecutive literal data contain larger literal counts. The decoder still accepts those
counts so the files remain readable, but decoders that enforce the limit reject them;
re-encoding the original image produces a conforming file.

## Files from versions 1.2.0 to 1.3.1

Versions 1.2.0 to 1.3.1 dropped the 16 header bytes when encoding raw Mode 2 sectors and
had the decoder emit 2352-byte sectors with sequential addresses generated from 00:02:00.
Archives they produced still decode, but as the format specifies: each Mode 2 record
yields 2336 bytes, so the output is a MODE2/2336 image rather than a 2352-byte image with
invented headers. The original addresses were never stored, so no decoder can recover
them.

`unecm --mode2-2352` reproduces what those versions wrote: every Mode 2 record expands to
a 2352-byte sector whose sync, address, and mode are regenerated, with the address
counted from 00:02:00 over all output bytes, literal runs included. For an image whose
addresses were sequential (the common case for a single-track disc) this restores the raw
image byte for byte. The option is only meant for such archives: a stream in the layout
above already carries its headers as literals, and expanding its records would duplicate
them.

The option is never applied automatically, because an archive written by those versions
from a raw Mode 2 image cannot be told apart from a spec-conformant archive describing a
MODE2/2336 image. Conversely, files written by 1.3.2 or later (or by the original `ecm`)
from raw Mode 2 images do not decode correctly with the 1.2.0 to 1.3.1 decoders: those
decoders add a header the stream already stores, producing 16 extra bytes per sector.
Version 1.1.0 and the original `unecm` decode them correctly.

The procedure for identifying and decoding such archives is in
[Decode archives from old ECM versions](../how-to/decode-old-archives.md).
