# How ECM works

## Why CD images carry redundant data

People who distribute images of CD-ROMs commonly create a raw copy in BIN/CUE (CDRWin),
CDI (DiscJuggler), NRG (Nero), or CCD (CloneCD) format and then compress that image with
an archive format such as RAR, hoping for a smaller file to transfer.

Raw CD images, however, contain both the useful sector data and the subcode data,
including the ECC (Error Correction Code) and EDC (Error Detection Code) of every sector.
For most sectors that subcode data is merely redundant: RAR already verifies with CRC,
and the transport layer detects errors, so shipping the redundant bytes serves no
purpose. Worse, ECC/EDC data is very poorly modeled by Lempel-Ziv compression schemes
such as the one RAR uses, which makes it effectively incompressible.

The insight behind ECM is that ECC/EDC bytes are not arbitrary: they are a deterministic
function of the sector contents. A format that models that function does not need to
store them at all — it regenerates them exactly when decoding. That is what ECM does,
and it is why a `.ecm` archive is smaller than the same image compressed with a
general-purpose archiver.

## What gets stored, and what gets reconstructed

| Sector Type | Raw Size | Stored | Savings | Description |
|-------------|----------|--------|---------|-------------|
| Mode 1 | 2352 B | 2051 B | ~13% | Standard data with full ECC/EDC |
| Mode 2 Form 1 | 2352 B | 2068 B | ~12% | XA data with ECC/EDC; the 16-byte sync/header is kept as literal bytes |
| Mode 2 Form 2 | 2352 B | 2344 B | <1% | XA audio/video (EDC only); the 16-byte sync/header is kept as literal bytes |
| Literal | varies | varies | 0% | Non-standard data, stored as-is |

For Mode 1, the sync pattern, mode byte, EDC, and ECC are all regenerated on decode. For
Mode 2 Form 1, the redundant flags, EDC, and ECC are regenerated, while the sync and
address bytes — which the format has no field for — are stored literally. Mode 2 Form 2
carries no ECC, so only its EDC is regenerated. Typical game discs shrink by 10–15%.

## Finding sectors anywhere

The encoder looks for a sector at every byte offset, not only at multiples of 2352. At
each offset it first tests for a raw 2352-byte sector with a sync pattern, then for a
header-less 2336-byte Mode 2 body, and stores as literal bytes anything that passes
neither test. This is what keeps images with an odd-sized prefix, MODE2/2336 images, and
dumps with extra bytes such as subchannel data after each sector compressible — as long
as the bytes in between fail the sector checks, they cannot be mistaken for data that
should be modeled.

A region of 2336 zero bytes is a valid Mode 2 Form 1 body, so zero padding is stored as
type 2 records; it still decodes exactly.

## Batch and streaming

The tool picks its processing mode from the input:

| Mode | Input Type | Buffer | Best For |
|------|-----------|--------|----------|
| Batch | Regular files | ~1 MB | Large files, best compression |
| Streaming | stdin/pipes | ~256 KB | Pipelines |

Both modes scan at every byte offset and produce files any ECM decoder can read. Batch
mode can seek and therefore groups consecutive same-type sectors into single records for
better compression ratios. Streaming handles the same detection with a smaller buffer and
writes records as it goes. Use regular files when you can; use pipes when the image comes
from or goes to another process.

## What does not shrink

A "cooked" ISO file stores 2048 bytes per sector and contains no ECC/EDC at all. There is
nothing for ECM to model, so the file is stored as literal bytes and sees no size
reduction — but it still round-trips exactly like any other input.

## Performance

Example results on an Apple M1:

```
EDC Computation:     ~370 MB/s
ECC Generation:      ~220,000 sectors/sec
Sector Detection:    ~104,000 sectors/sec
Encode Throughput:   ~120 MB/s
Decode Throughput:   ~120 MB/s
```

Run the benchmark suite on your own machine with:

```bash
just benchmark
```
