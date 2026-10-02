# ECM file format specification

ECM (Error Code Modeler) format, version 1.0, written by Neill Corlett. This
implementation follows the specification exactly; see
[Compatibility](../explanation/compatibility.md) for implementation behavior, version
history, and interoperability with other ECM tools.

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

## Credits

The ECM format and this specification were written by Neill Corlett
(corlett@lfx.org).
