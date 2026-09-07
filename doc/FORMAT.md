# ECM (Error Code Modeler) Format Specification v1.0

*Written by Neill Corlett*

## Introduction

For many people who distribute images of CD-ROMs, the common practice is to
create a raw copy in either BIN/CUE (CDRWin), CDI (DiscJuggler), NRG (Nero),
or CCD (CloneCD) format, and then compress that image using an archive format
such as RAR. The goal is to reduce the CD image to the smallest possible
size for quicker Internet transfers. However, there is a major problem with
this technique.

Raw CD images contain both the useful sector data as well as subcodes,
including ECC and EDC (Error Correction/Detection Codes). In many cases,
it's necessary to include all subcode information to ensure a complete and
accurate CD image. However, most raw CD images include a large number of
"plain" sectors, for which the subcode data is merely redundant. Since the
RAR format includes CRC verification and the TCP/IP transport layer also
includes error detection, there is no purpose to transferring this redundant
data.

Making the problem worse, the ECC/EDC data in a typical CD sector is very
poorly modeled by typical Lempel-Ziv compression schemes (such as the one
used by RAR). This means that, for practical purposes, ECC/EDC data is
uncompressible.

However, with a simple algorithm, the ECC/EDC data can be reconstructed
for a typical CD sector. What's needed is a compressed file format which
uses the ECC/EDC algorithms as a model to eliminate the redundant data.
This is ECM, the Error Code Modeler format.

## Sector Types

There are three basic types of CD data sectors:

### Type #1: Mode 1

```
       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 01
0010h [---DATA...
...
0800h                                     ...DATA---]
0810h [---EDC---] 00 00 00 00 00 00 00 00 [---ECC...
...
0920h                                      ...ECC---]
```

### Type #2: Mode 2 (XA), form 1

```
       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
0010h [--FLAGS--] [--FLAGS--] [---DATA...
...
0810h             ...DATA---] [---EDC---] [---ECC...
...
0920h                                      ...ECC---]
```

### Type #3: Mode 2 (XA), form 2

```
       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
0010h [--FLAGS--] [--FLAGS--] [---DATA...
...
0920h                         ...DATA---] [---EDC---]
```

### Key

| Field | Description |
|-------|-------------|
| ADDR | Sector address, encoded as minutes:seconds:frames in BCD |
| FLAGS | Used in Mode 2 (XA) sectors describing the type of sector; repeated twice for redundancy |
| DATA | Area of the sector which contains the actual data itself |
| EDC | Error Detection Code |
| ECC | Error Correction Code |

The ECM format uses these three basic types to model CD sectors.

## The ECM File Format

The first 4 bytes are the magic identifier: `45 43 4D 00`, or `"ECM"`.

After this comes an arbitrary (can be infinitely long) number of records.

Each record has 3 parts: **Type** (0...3), **Count** (1...2^31-1), and **Data**.

| Type | Description |
|------|-------------|
| 0 | Literal bytes follow; length is indicated by Count |
| 1 | Sectors of type #1 follow; Count tells how many |
| 2 | Sectors of type #2 follow; Count tells how many |
| 3 | Sectors of type #3 follow; Count tells how many |

### Type/Count Encoding

The Type and Count are encoded first, in the following format.
(Second through fifth bytes are OPTIONAL.)

```
 first byte   second byte   third byte   fourth byte   fifth byte
  AaaaaaTT     Bbbbbbbb      Cccccccc     Dddddddd      00eeeeee
```

| Bit(s) | Description |
|--------|-------------|
| T | Type |
| a | Bits 0-4 of Count |
| A | Set if the second byte exists |
| b | Bits 5-11 of Count |
| B | Set if the third byte exists |
| c | Bits 12-18 of Count |
| C | Set if the fourth byte exists |
| d | Bits 19-25 of Count |
| D | Set if the fifth byte exists |
| e | Bits 26-31 of Count |

> **NOTE:** The Count value that is encoded is the actual Count minus 1.

Type 0 and a count of exactly 2^32 (meaning it's encoded as `FFFFFFFF`)
indicates that there are no more records. Immediately following this
marker is a 4-byte EDC for the entire original (unencoded) file. This is
computed using the same algorithm as the EDC for a CD sector. After this,
the ECM file ends.

Counts of 2^31 and higher are invalid.

## Sector Type #1

Stored in the ECM file as follows:

| Size | Content |
|------|---------|
| 3 bytes | ADDR |
| 2048 bytes | DATA |

This expands to a complete 2352-byte Mode 1 sector.

The sync, reserved, mode (01), EDC, and ECC bytes are reconstructed upon
decoding.

## Sector Type #2

Stored in the ECM file as follows:

| Size | Content |
|------|---------|
| 4 bytes | FLAGS |
| 2048 bytes | DATA |

This expands to a 2336-byte Mode 2 Form 1 sector. The sync, address, and
mode bytes are NOT included.

The redundant flags, EDC, and ECC bytes are reconstructed upon decoding.

## Sector Type #3

Stored in the ECM file as follows:

| Size | Content |
|------|---------|
| 4 bytes | FLAGS |
| 2324 bytes | DATA |

This expands to a 2336-byte Mode 2 Form 2 sector. The sync, address, and
mode bytes are NOT included.

The redundant flags and EDC bytes are reconstructed upon decoding.

## Implementation Notes

Everything above is Neill Corlett's original specification. This implementation follows it
exactly, including the record semantics for Mode 2.

### Raw Mode 2 Sectors

A type 2 or type 3 record expands to the **2336-byte** Mode 2 body (flags through EDC/ECC).
It has no field for sync, address, or mode. When the encoder meets a raw 2352-byte Mode 2
sector it therefore emits two records, exactly as the original `ecm` did:

| Record | Content |
|--------|---------|
| Type 0, count 16 | Sync pattern, MSF address, and mode byte, stored verbatim |
| Type 2 or 3, count 1 | FLAGS + DATA for the 2336-byte body |

The original address survives the roundtrip because it is stored rather than regenerated,
and the decoded image is byte-for-byte identical to the input. Streams written by this tool
decode with the original `unecm`, and streams written by the original `ecm` decode with this
tool.

### Files From Versions 1.2.0 to 1.3.1

Those versions dropped the 16 header bytes when encoding raw Mode 2 sectors and had the
decoder emit 2352-byte sectors with sequential addresses generated from 00:02:00. Files they
produced still decode, but as the format specifies: each Mode 2 record yields 2336 bytes, so
the output is a MODE2/2336 image rather than a 2352-byte image with invented headers. The
original addresses were never stored, so no decoder can recover them.

## Contact

- **Email:** corlett@lfx.org
