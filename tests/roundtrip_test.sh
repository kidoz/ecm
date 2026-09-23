#!/bin/bash
# Integration test: roundtrip encoding/decoding
# Tests that: original -> ecm -> unecm -> should equal original

set -e

ECM_BIN="${1:-./build/ecm}"
UNECM_BIN="${2:-./build/unecm}"
TEST_DIR=$(mktemp -d)
# Native tools (python3 on Windows) cannot open MSYS-style /tmp paths; use a mixed path
if command -v cygpath >/dev/null 2>&1; then
    TEST_DIR=$(cygpath -m "$TEST_DIR")
fi

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

echo "=== ECM/UNECM Roundtrip Test ==="
echo "ECM binary: $ECM_BIN"
echo "UNECM binary: $UNECM_BIN"
echo "Test directory: $TEST_DIR"

# Verify binaries exist
if [ ! -x "$ECM_BIN" ]; then
    echo "ERROR: ecm binary not found or not executable: $ECM_BIN"
    exit 1
fi

if [ ! -x "$UNECM_BIN" ]; then
    echo "ERROR: unecm binary not found or not executable: $UNECM_BIN"
    exit 1
fi

# Some tests run from inside the test directory, so the binaries need absolute paths
ECM_BIN="$(cd "$(dirname "$ECM_BIN")" && pwd)/$(basename "$ECM_BIN")"
UNECM_BIN="$(cd "$(dirname "$UNECM_BIN")" && pwd)/$(basename "$UNECM_BIN")"

# Fixtures are built with Python 3; some Windows installs only provide it as 'python'
PYTHON="${PYTHON:-python3}"
if ! "$PYTHON" -c "import sys; assert sys.version_info >= (3,)" > /dev/null 2>&1; then
    PYTHON=python
fi
if ! "$PYTHON" -c "import sys; assert sys.version_info >= (3,)" > /dev/null 2>&1; then
    echo "ERROR: Python 3 not found (tried python3 and python)"
    exit 1
fi

generate_sectors() {
"$PYTHON" - <<'PY' "$@"
import sys, struct

# Constants
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])
SECTOR_SIZE_RAW = 2352
SECTOR_USER_DATA = 2048
MODE2_FORM1_DATA_SIZE = 2052  # subheader copy (4) + user data (2048)
MODE2_FORM2_DATA_SIZE = 2328  # subheader copy (4) + user data (2324)

ECC_P_MAJOR = 86
ECC_P_MINOR = 24
ECC_P_MULT = 2
ECC_P_INC = 86
ECC_Q_MAJOR = 52
ECC_Q_MINOR = 43
ECC_Q_MULT = 86
ECC_Q_INC = 88
ECC_DATA_OFFSET = 0x0C
OFFSET_MODE1_EDC = 0x810
OFFSET_MODE1_RESERVED = 0x814
OFFSET_MODE1_ECC_P = 0x81C
OFFSET_MODE1_ECC_Q = 0x8C8
OFFSET_MODE2_SUBHEADER = 0x10
MODE2_EDC_OFFSET = 0x808
MODE2_FORM2_EDC_OFFSET = 0x91C
OFFSET_MODE2_FORM1_EDC = 0x818
OFFSET_MODE2_FORM2_EDC = 0x92C

def init_tables():
    ecc_f = [0] * 256
    ecc_b = [0] * 256
    edc = [0] * 256
    for i in range(256):
        j = (i << 1) ^ (0x11D if (i & 0x80) else 0)
        ecc_f[i] = j & 0xFF
        ecc_b[i ^ j] = i
        edc_val = i
        for _ in range(8):
            edc_val = (edc_val >> 1) ^ (0xD8018001 if (edc_val & 1) else 0)
        edc[i] = edc_val & 0xFFFFFFFF
    return ecc_f, ecc_b, edc

ECC_F_LUT, ECC_B_LUT, EDC_LUT = init_tables()

def edc_compute(edc, data):
    for b in data:
        edc = ((edc >> 8) ^ EDC_LUT[(edc ^ b) & 0xFF]) & 0xFFFFFFFF
    return edc

def edc_bytes(data):
    val = edc_compute(0, data)
    return struct.pack("<I", val)

def ecc_compute_block(src, major_count, minor_count, major_mult, minor_inc):
    size = major_count * minor_count
    dest = bytearray(major_count * 2)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        ecc_a = 0
        ecc_b = 0
        for _ in range(minor_count):
            temp = src[index]
            index += minor_inc
            if index >= size:
                index -= size
            ecc_a ^= temp
            ecc_b ^= temp
            ecc_a = ECC_F_LUT[ecc_a]
        ecc_a = ECC_B_LUT[ECC_F_LUT[ecc_a] ^ ecc_b]
        dest[major] = ecc_a
        dest[major + major_count] = ecc_a ^ ecc_b
    return dest

def ecc_generate(sector, zeroaddress):
    if zeroaddress:
        addr = sector[12:16]
        sector[12:16] = b"\x00" * 4
    # ECC P
    ecc_p = ecc_compute_block(sector[ECC_DATA_OFFSET:], ECC_P_MAJOR, ECC_P_MINOR, ECC_P_MULT, ECC_P_INC)
    sector[OFFSET_MODE1_ECC_P:OFFSET_MODE1_ECC_P + len(ecc_p)] = ecc_p
    # ECC Q
    ecc_q = ecc_compute_block(sector[ECC_DATA_OFFSET:], ECC_Q_MAJOR, ECC_Q_MINOR, ECC_Q_MULT, ECC_Q_INC)
    sector[OFFSET_MODE1_ECC_Q:OFFSET_MODE1_ECC_Q + len(ecc_q)] = ecc_q
    if zeroaddress:
        sector[12:16] = addr

def make_mode1(msf):
    sector = bytearray(SECTOR_SIZE_RAW)
    sector[0:12] = SYNC
    sector[12:15] = msf
    sector[15] = 0x01
    for i in range(SECTOR_USER_DATA):
        sector[0x10 + i] = i & 0xFF
    # EDC over 0x000-0x80F
    sector[OFFSET_MODE1_EDC:OFFSET_MODE1_EDC + 4] = edc_bytes(sector[:OFFSET_MODE1_EDC])
    # Reserved zeroed
    # ECC
    ecc_generate(sector, False)
    return bytes(sector)

def make_mode2_form1(msf):
    sector = bytearray(SECTOR_SIZE_RAW)
    sector[0:12] = SYNC
    sector[12:15] = msf
    sector[15] = 0x02
    # subheader + copy
    sector[0x10:0x14] = bytes([0x00, 0x00, 0x08, 0x00])
    sector[0x14:0x18] = sector[0x10:0x14]
    for i in range(SECTOR_USER_DATA):
        sector[0x18 + i] = (i * 3) & 0xFF
    # EDC over bytes 0x010-0x817 (2048+8)
    sector[OFFSET_MODE2_FORM1_EDC:OFFSET_MODE2_FORM1_EDC + 4] = edc_bytes(
        sector[OFFSET_MODE2_SUBHEADER:OFFSET_MODE2_SUBHEADER + MODE2_EDC_OFFSET]
    )
    ecc_generate(sector, True)
    return bytes(sector)

def make_mode2_form2(msf):
    sector = bytearray(SECTOR_SIZE_RAW)
    sector[0:12] = SYNC
    sector[12:15] = msf
    sector[15] = 0x02
    sector[0x10:0x14] = bytes([0x00, 0x00, 0x20, 0x00])
    sector[0x14:0x18] = sector[0x10:0x14]
    for i in range(2324):
        sector[0x18 + i] = (i * 5) & 0xFF
    # EDC over bytes 0x010-0x92B (0x91C bytes)
    sector[OFFSET_MODE2_FORM2_EDC:OFFSET_MODE2_FORM2_EDC + 4] = edc_bytes(
        sector[OFFSET_MODE2_SUBHEADER:OFFSET_MODE2_SUBHEADER + MODE2_FORM2_EDC_OFFSET]
    )
    # Note: Mode 2 Form 2 has NO ECC, only EDC - do not call ecc_generate here
    return bytes(sector)

def msf_from_index(index):
    frame = index + 150
    f = frame % 75
    frame //= 75
    s = frame % 60
    m = frame // 60
    # BCD
    return bytes([(m // 10 << 4) | (m % 10), (s // 10 << 4) | (s % 10), (f // 10 << 4) | (f % 10)])

def main():
    mode = sys.argv[1]
    count = int(sys.argv[2])
    path = sys.argv[3]
    start = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    builders = {
        "mode1": make_mode1,
        "mode2f1": make_mode2_form1,
        "mode2f2": make_mode2_form2,
    }
    build = builders[mode]
    with open(path, "wb") as f:
        for i in range(count):
            f.write(build(msf_from_index(start + i)))

if __name__ == "__main__":
    main()
PY
}

# Build an ECM stream from a raw Mode 2 image without using the encoder under test.
# with-header: the upstream layout (16-byte literal + type 2/3 record per sector), whose
#              decoded output must equal the raw image
# headerless:  type 2/3 records only, describing 2336-byte sectors; also writes the
#              expected 2336-byte-per-sector output to <expected>
# usage: build_spec_ecm with-header|headerless <raw.bin> <out.ecm> [<expected>]
build_spec_ecm() {
"$PYTHON" - <<'PY' "$@"
import struct, sys

def init_edc():
    lut = [0] * 256
    for i in range(256):
        v = i
        for _ in range(8):
            v = (v >> 1) ^ (0xD8018001 if (v & 1) else 0)
        lut[i] = v & 0xFFFFFFFF
    return lut

EDC_LUT = init_edc()

def edc_compute(edc, data):
    for b in data:
        edc = ((edc >> 8) ^ EDC_LUT[(edc ^ b) & 0xFF]) & 0xFFFFFFFF
    return edc

def type_count(t, count):
    c = (count - 1) & 0xFFFFFFFF
    out = bytes([((c >= 32) << 7) | ((c & 31) << 2) | t])
    c >>= 5
    while c:
        out += bytes([((c >= 128) << 7) | (c & 127)])
        c >>= 7
    return out

layout, raw_path, ecm_path = sys.argv[1:4]
raw = open(raw_path, "rb").read()
assert len(raw) % 2352 == 0, "raw image must be whole 2352-byte sectors"

ecm = bytearray(b"ECM\x00")
covered = bytearray()
for off in range(0, len(raw), 2352):
    sector = raw[off:off + 2352]
    form2 = sector[0x12] & 0x20
    rec_type, payload_len = (3, 0x918) if form2 else (2, 0x804)
    if layout == "with-header":
        ecm += type_count(0, 16) + sector[:16]
        covered += sector
    else:
        covered += sector[0x10:]
    ecm += type_count(rec_type, 1) + sector[0x14:0x14 + payload_len]
ecm += type_count(0, 0) + struct.pack("<I", edc_compute(0, covered))
open(ecm_path, "wb").write(ecm)
if len(sys.argv) > 4:
    open(sys.argv[4], "wb").write(covered)
PY
}

# Test 1: Mode 1 sector (2352 bytes) with valid ECC/EDC
echo "--- Test 1: Mode 1 sector roundtrip ---"
MODE1_FILE="$TEST_DIR/mode1.bin"
generate_sectors mode1 10 "$MODE1_FILE"

ORIGINAL_SUM=$(sha256sum "$MODE1_FILE" | cut -d' ' -f1)
echo "Original file checksum: $ORIGINAL_SUM"

# Encode
"$ECM_BIN" "$MODE1_FILE" "$TEST_DIR/mode1.bin.ecm" 2>&1 || true
if [ ! -f "$TEST_DIR/mode1.bin.ecm" ]; then
    echo "ERROR: ECM encoding failed"
    exit 1
fi
echo "Encoded to: $TEST_DIR/mode1.bin.ecm"

# Decode
"$UNECM_BIN" "$TEST_DIR/mode1.bin.ecm" "$TEST_DIR/mode1_decoded.bin" 2>&1 || true
if [ ! -f "$TEST_DIR/mode1_decoded.bin" ]; then
    echo "ERROR: UNECM decoding failed"
    exit 1
fi

DECODED_SUM=$(sha256sum "$TEST_DIR/mode1_decoded.bin" | cut -d' ' -f1)
echo "Decoded file checksum: $DECODED_SUM"

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Mode 1 roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    echo "  Original: $ORIGINAL_SUM"
    echo "  Decoded:  $DECODED_SUM"
    exit 1
fi

# Test 2: Random data (literal bytes, type 0)
echo ""
echo "--- Test 2: Random/literal data roundtrip ---"
RANDOM_FILE="$TEST_DIR/random.bin"
dd if=/dev/urandom of="$RANDOM_FILE" bs=1024 count=100 2>/dev/null

ORIGINAL_SUM=$(sha256sum "$RANDOM_FILE" | cut -d' ' -f1)
echo "Original file checksum: $ORIGINAL_SUM"

# Encode
"$ECM_BIN" "$RANDOM_FILE" "$TEST_DIR/random.bin.ecm" 2>&1 || true
if [ ! -f "$TEST_DIR/random.bin.ecm" ]; then
    echo "ERROR: ECM encoding failed"
    exit 1
fi
echo "Encoded to: $TEST_DIR/random.bin.ecm"

# Decode
"$UNECM_BIN" "$TEST_DIR/random.bin.ecm" "$TEST_DIR/random_decoded.bin" 2>&1 || true
if [ ! -f "$TEST_DIR/random_decoded.bin" ]; then
    echo "ERROR: UNECM decoding failed"
    exit 1
fi

DECODED_SUM=$(sha256sum "$TEST_DIR/random_decoded.bin" | cut -d' ' -f1)
echo "Decoded file checksum: $DECODED_SUM"

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Random data roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 3: Empty file (edge case)
echo ""
echo "--- Test 3: Empty file handling ---"
EMPTY_FILE="$TEST_DIR/empty.bin"
touch "$EMPTY_FILE"

# Encode empty file
"$ECM_BIN" "$EMPTY_FILE" "$TEST_DIR/empty.bin.ecm" 2>&1 || true
if [ ! -f "$TEST_DIR/empty.bin.ecm" ]; then
    echo "ERROR: ECM encoding failed for empty file"
    exit 1
fi

# Decode
"$UNECM_BIN" "$TEST_DIR/empty.bin.ecm" "$TEST_DIR/empty_decoded.bin" 2>&1 || true
if [ ! -f "$TEST_DIR/empty_decoded.bin" ]; then
    echo "ERROR: UNECM decoding failed for empty file"
    exit 1
fi

# Cross-platform file size: use wc -c (works on both Linux and macOS)
ORIGINAL_SIZE=$(wc -c < "$EMPTY_FILE" | tr -d ' ')
DECODED_SIZE=$(wc -c < "$TEST_DIR/empty_decoded.bin" | tr -d ' ')
if [ "$ORIGINAL_SIZE" = "$DECODED_SIZE" ]; then
    echo "PASS: Empty file roundtrip successful"
else
    echo "FAIL: Sizes don't match! Original: $ORIGINAL_SIZE, Decoded: $DECODED_SIZE"
    exit 1
fi

# Test 4: Single byte file
echo ""
echo "--- Test 4: Single byte file ---"
SINGLE_FILE="$TEST_DIR/single.bin"
printf '\x42' > "$SINGLE_FILE"

ORIGINAL_SUM=$(sha256sum "$SINGLE_FILE" | cut -d' ' -f1)

"$ECM_BIN" "$SINGLE_FILE" "$TEST_DIR/single.bin.ecm" 2>&1 || true
"$UNECM_BIN" "$TEST_DIR/single.bin.ecm" "$TEST_DIR/single_decoded.bin" 2>&1 || true

DECODED_SUM=$(sha256sum "$TEST_DIR/single_decoded.bin" | cut -d' ' -f1)
if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Single byte roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 5: Mode 2 Form 1 sector (with valid ECC/EDC)
echo ""
echo "--- Test 5: Mode 2 Form 1 sector roundtrip ---"
MODE2F1_FILE="$TEST_DIR/mode2f1.bin"
generate_sectors mode2f1 5 "$MODE2F1_FILE"

ORIGINAL_SUM=$(sha256sum "$MODE2F1_FILE" | cut -d' ' -f1)
"$ECM_BIN" "$MODE2F1_FILE" "$TEST_DIR/mode2f1.bin.ecm" 2>&1 || true
"$UNECM_BIN" "$TEST_DIR/mode2f1.bin.ecm" "$TEST_DIR/mode2f1_decoded.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/mode2f1_decoded.bin" | cut -d' ' -f1)

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Mode 2 Form 1 roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 6: Mode 2 Form 2 sector (with valid EDC)
echo ""
echo "--- Test 6: Mode 2 Form 2 sector roundtrip ---"
MODE2F2_FILE="$TEST_DIR/mode2f2.bin"
generate_sectors mode2f2 5 "$MODE2F2_FILE"

ORIGINAL_SUM=$(sha256sum "$MODE2F2_FILE" | cut -d' ' -f1)
"$ECM_BIN" "$MODE2F2_FILE" "$TEST_DIR/mode2f2.bin.ecm" 2>&1 || true
"$UNECM_BIN" "$TEST_DIR/mode2f2.bin.ecm" "$TEST_DIR/mode2f2_decoded.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/mode2f2_decoded.bin" | cut -d' ' -f1)

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Mode 2 Form 2 roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 7: Large file (multiple MB)
echo ""
echo "--- Test 7: Large file roundtrip ---"
LARGE_FILE="$TEST_DIR/large.bin"
dd if=/dev/urandom of="$LARGE_FILE" bs=1M count=5 2>/dev/null

ORIGINAL_SUM=$(sha256sum "$LARGE_FILE" | cut -d' ' -f1)
"$ECM_BIN" "$LARGE_FILE" "$TEST_DIR/large.bin.ecm" 2>&1 || true
"$UNECM_BIN" "$TEST_DIR/large.bin.ecm" "$TEST_DIR/large_decoded.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/large_decoded.bin" | cut -d' ' -f1)

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Large file roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 8: Corrupted ECM file detection
echo ""
echo "--- Test 8: Corrupted ECM file detection ---"
CORRUPT_FILE="$TEST_DIR/corrupt.bin"
dd if=/dev/urandom of="$CORRUPT_FILE" bs=1024 count=10 2>/dev/null
"$ECM_BIN" "$CORRUPT_FILE" "$TEST_DIR/corrupt.bin.ecm" 2>&1 || true

# Corrupt the EDC checksum (last 4 bytes)
"$PYTHON" -c "
with open('$TEST_DIR/corrupt.bin.ecm', 'r+b') as f:
    f.seek(-4, 2)
    f.write(bytes([0xFF, 0xFF, 0xFF, 0xFF]))
"

# Decoding should fail
if "$UNECM_BIN" "$TEST_DIR/corrupt.bin.ecm" "$TEST_DIR/corrupt_decoded.bin" 2>&1 | grep -q -i "edc"; then
    echo "PASS: Corrupted file detected correctly"
else
    # Check if decoding failed (non-zero exit or file not created correctly)
    if [ ! -f "$TEST_DIR/corrupt_decoded.bin" ]; then
        echo "PASS: Corrupted file rejected (no output)"
    else
        CORRUPT_SUM=$(sha256sum "$CORRUPT_FILE" | cut -d' ' -f1)
        DECODED_SUM=$(sha256sum "$TEST_DIR/corrupt_decoded.bin" | cut -d' ' -f1)
        if [ "$CORRUPT_SUM" != "$DECODED_SUM" ]; then
            echo "PASS: Corrupted file detected (checksum mismatch)"
        else
            echo "FAIL: Corrupted file not detected!"
            exit 1
        fi
    fi
fi
# An image that failed verification must not be left under the requested name
if [ -e "$TEST_DIR/corrupt_decoded.bin" ]; then
    echo "FAIL: unecm left the unverified output behind"
    exit 1
fi

# Test 9: Invalid magic header
echo ""
echo "--- Test 9: Invalid magic header detection ---"
BAD_MAGIC="$TEST_DIR/bad_magic.ecm"
printf 'XCM\x00' > "$BAD_MAGIC"
dd if=/dev/urandom bs=100 count=1 >> "$BAD_MAGIC" 2>/dev/null

if "$UNECM_BIN" "$BAD_MAGIC" "$TEST_DIR/bad_magic_decoded.bin" 2>&1 | grep -qi "header"; then
    echo "PASS: Invalid magic header detected"
else
    # Should have failed
    if [ ! -f "$TEST_DIR/bad_magic_decoded.bin" ] || [ ! -s "$TEST_DIR/bad_magic_decoded.bin" ]; then
        echo "PASS: Invalid magic header rejected"
    else
        echo "FAIL: Invalid magic header not detected!"
        exit 1
    fi
fi

# Test 10: Mode 2 sectors whose addresses do not start at 00:02:00.
# The decoder used to regenerate sequential addresses, so this only roundtripped by luck.
echo ""
echo "--- Test 10: Mode 2 roundtrip with non-sequential addresses ---"
M2ADDR_FILE="$TEST_DIR/mode2_addr.bin"
generate_sectors mode2f1 4 "$M2ADDR_FILE" 4500                # first sector at 01:02:00
generate_sectors mode2f2 3 "$TEST_DIR/mode2f2_addr.bin" 9000  # 02:02:00
cat "$TEST_DIR/mode2f2_addr.bin" >> "$M2ADDR_FILE"

ORIGINAL_SUM=$(sha256sum "$M2ADDR_FILE" | cut -d' ' -f1)
"$ECM_BIN" "$M2ADDR_FILE" "$TEST_DIR/mode2_addr.bin.ecm" 2>&1 || true
"$UNECM_BIN" "$TEST_DIR/mode2_addr.bin.ecm" "$TEST_DIR/mode2_addr_decoded.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/mode2_addr_decoded.bin" | cut -d' ' -f1)

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Mode 2 addresses preserved"
else
    echo "FAIL: Checksums don't match!"
    echo "  Original: $ORIGINAL_SUM"
    echo "  Decoded:  $DECODED_SUM"
    exit 1
fi

# Test 11: Mixed image - every sector type plus a partial trailing sector
echo ""
echo "--- Test 11: Mixed sector types with partial tail ---"
MIXED_FILE="$TEST_DIR/mixed.bin"
generate_sectors mode1 3 "$MIXED_FILE" 150
generate_sectors mode2f1 2 "$TEST_DIR/mixed_f1.bin" 153
generate_sectors mode2f2 2 "$TEST_DIR/mixed_f2.bin" 155
cat "$TEST_DIR/mixed_f1.bin" "$TEST_DIR/mixed_f2.bin" >> "$MIXED_FILE"
dd if=/dev/urandom bs=100 count=1 >> "$MIXED_FILE" 2>/dev/null

ORIGINAL_SUM=$(sha256sum "$MIXED_FILE" | cut -d' ' -f1)
"$ECM_BIN" "$MIXED_FILE" "$TEST_DIR/mixed.bin.ecm" 2>&1 || true
"$UNECM_BIN" "$TEST_DIR/mixed.bin.ecm" "$TEST_DIR/mixed_decoded.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/mixed_decoded.bin" | cut -d' ' -f1)

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Mixed image roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 12: Streaming mode through stdin/stdout on the mixed image
echo ""
echo "--- Test 12: Streaming roundtrip via stdin/stdout ---"
"$ECM_BIN" - - < "$MIXED_FILE" > "$TEST_DIR/mixed_stream.ecm" 2>/dev/null
"$UNECM_BIN" - - < "$TEST_DIR/mixed_stream.ecm" > "$TEST_DIR/mixed_stream_decoded.bin" 2>/dev/null
DECODED_SUM=$(sha256sum "$TEST_DIR/mixed_stream_decoded.bin" | cut -d' ' -f1)

if [ "$ORIGINAL_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Streaming roundtrip successful"
else
    echo "FAIL: Checksums don't match!"
    exit 1
fi

# Test 13: Spec-conformant streams built independently of the encoder.
# A type 2/3 record must expand to 2336 bytes: with the literal header in front that
# reproduces the raw image; without it the output is the header-less 2336-byte image.
echo ""
echo "--- Test 13: Decoding spec-conformant ECM streams ---"
build_spec_ecm with-header "$M2ADDR_FILE" "$TEST_DIR/spec_raw.ecm"
"$UNECM_BIN" "$TEST_DIR/spec_raw.ecm" "$TEST_DIR/spec_raw_decoded.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/spec_raw_decoded.bin" | cut -d' ' -f1)
ORIGINAL_SUM=$(sha256sum "$M2ADDR_FILE" | cut -d' ' -f1)
if [ "$ORIGINAL_SUM" != "$DECODED_SUM" ]; then
    echo "FAIL: literal header + type 2/3 record did not reproduce the raw image"
    echo "  Expected: $ORIGINAL_SUM ($(wc -c < "$M2ADDR_FILE" | tr -d ' ') bytes)"
    echo "  Decoded:  $DECODED_SUM ($(wc -c < "$TEST_DIR/spec_raw_decoded.bin" | tr -d ' ') bytes)"
    exit 1
fi

build_spec_ecm headerless "$M2ADDR_FILE" "$TEST_DIR/spec_2336.ecm" "$TEST_DIR/spec_2336_expected.bin"
"$UNECM_BIN" "$TEST_DIR/spec_2336.ecm" "$TEST_DIR/spec_2336_decoded.bin" 2>&1 || true
EXPECTED_SUM=$(sha256sum "$TEST_DIR/spec_2336_expected.bin" | cut -d' ' -f1)
DECODED_SUM=$(sha256sum "$TEST_DIR/spec_2336_decoded.bin" | cut -d' ' -f1)
if [ "$EXPECTED_SUM" = "$DECODED_SUM" ]; then
    echo "PASS: Spec-conformant streams decode correctly"
else
    echo "FAIL: header-less type 2/3 records did not expand to 2336-byte sectors"
    echo "  Expected: $EXPECTED_SUM ($(wc -c < "$TEST_DIR/spec_2336_expected.bin" | tr -d ' ') bytes)"
    echo "  Decoded:  $DECODED_SUM ($(wc -c < "$TEST_DIR/spec_2336_decoded.bin" | tr -d ' ') bytes)"
    exit 1
fi

# Test 14: Output that aliases the input must be refused, leaving the input intact
echo ""
echo "--- Test 14: Identical input and output paths are refused ---"
SAME_FILE="$TEST_DIR/same.bin"
cp "$MODE1_FILE" "$SAME_FILE"
SUM_BEFORE=$(sha256sum "$SAME_FILE" | cut -d' ' -f1)

if "$ECM_BIN" "$SAME_FILE" "$SAME_FILE" > /dev/null 2>&1; then
    echo "FAIL: ecm accepted identical input and output paths"
    exit 1
fi
# ln -s silently copies on Windows without symlink privileges; only test a real link
ln -s "same.bin" "$TEST_DIR/same_alias.bin" 2>/dev/null || true
if [ -L "$TEST_DIR/same_alias.bin" ]; then
    if "$ECM_BIN" "$SAME_FILE" "$TEST_DIR/same_alias.bin" > /dev/null 2>&1; then
        echo "FAIL: ecm accepted a symlink alias of the input as output"
        exit 1
    fi
else
    echo "SKIP: symlinks unavailable, alias check skipped"
fi
SUM_AFTER=$(sha256sum "$SAME_FILE" | cut -d' ' -f1)
if [ "$SUM_BEFORE" != "$SUM_AFTER" ]; then
    echo "FAIL: ecm truncated its own input"
    exit 1
fi

SAME_ECM="$TEST_DIR/same.ecm"
cp "$TEST_DIR/mode1.bin.ecm" "$SAME_ECM"
SUM_BEFORE=$(sha256sum "$SAME_ECM" | cut -d' ' -f1)
if "$UNECM_BIN" "$SAME_ECM" "$SAME_ECM" > /dev/null 2>&1; then
    echo "FAIL: unecm accepted identical input and output paths"
    exit 1
fi
SUM_AFTER=$(sha256sum "$SAME_ECM" | cut -d' ' -f1)
if [ "$SUM_BEFORE" = "$SUM_AFTER" ]; then
    echo "PASS: Aliased output refused, inputs intact"
else
    echo "FAIL: unecm truncated its own input"
    exit 1
fi

# Test 15: A read error on stdin is an error, not an empty input
echo ""
echo "--- Test 15: Unreadable stdin is rejected ---"
if "$ECM_BIN" - "$TEST_DIR/unreadable.ecm" < "$TEST_DIR" > /dev/null 2>&1; then
    echo "FAIL: ecm reported success on an unreadable stream"
    exit 1
else
    echo "PASS: Unreadable stdin rejected"
fi

# Test 16: --mode2-2352 restores an archive written by versions 1.2.0 to 1.3.1.
# Those versions stored raw Mode 2 sectors as header-less records and regenerated sequential
# addresses from 00:02:00 on decode, so a header-less stream of an image whose addresses are
# sequential must come back byte for byte with the option, and as 2336-byte bodies without.
echo ""
echo "--- Test 16: Legacy archive restored with --mode2-2352 ---"
LEGACY_RAW="$TEST_DIR/legacy.bin"
generate_sectors mode2f1 3 "$LEGACY_RAW" 0                   # 00:02:00 .. 00:02:02
generate_sectors mode2f2 2 "$TEST_DIR/legacy_f2.bin" 3       # 00:02:03 .. 00:02:04
cat "$TEST_DIR/legacy_f2.bin" >> "$LEGACY_RAW"
build_spec_ecm headerless "$LEGACY_RAW" "$TEST_DIR/legacy.ecm"

ORIGINAL_SUM=$(sha256sum "$LEGACY_RAW" | cut -d' ' -f1)
"$UNECM_BIN" --mode2-2352 "$TEST_DIR/legacy.ecm" "$TEST_DIR/legacy_restored.bin" 2>&1 || true
DECODED_SUM=$(sha256sum "$TEST_DIR/legacy_restored.bin" | cut -d' ' -f1)
if [ "$ORIGINAL_SUM" != "$DECODED_SUM" ]; then
    echo "FAIL: --mode2-2352 did not restore the raw image"
    echo "  Original: $ORIGINAL_SUM"
    echo "  Decoded:  $DECODED_SUM ($(wc -c < "$TEST_DIR/legacy_restored.bin" | tr -d ' ') bytes)"
    exit 1
fi
# Options may be combined in any order with the others
"$UNECM_BIN" --cue --mode2-2352 -v "$TEST_DIR/legacy.ecm" "$TEST_DIR/legacy_cue.bin" > /dev/null 2>&1 || true
if [ ! -f "$TEST_DIR/legacy_cue.bin.cue" ] || ! grep -q "MODE2/2352" "$TEST_DIR/legacy_cue.bin.cue"; then
    echo "FAIL: --mode2-2352 combined with --cue and -v did not produce a MODE2/2352 cue sheet"
    exit 1
fi
if "$UNECM_BIN" --no-such-option "$TEST_DIR/legacy.ecm" "$TEST_DIR/legacy_bad.bin" > /dev/null 2>&1; then
    echo "FAIL: unecm accepted an unknown option"
    exit 1
fi
echo "PASS: Legacy archive restored with --mode2-2352"

# Test 17: The CUE sheet is written next to the image and players resolve its FILE entry
# relative to the sheet, so the entry must be the bare file name. Writing the output path as
# typed made a sheet for outdir/restored.bin point at outdir/outdir/restored.bin.
echo ""
echo "--- Test 17: CUE sheet references the image by file name ---"
mkdir -p "$TEST_DIR/cue_abs" "$TEST_DIR/cue_rel"
# Absolute output path (on Windows it also carries a drive letter)
if ! "$UNECM_BIN" --cue "$TEST_DIR/mode1.bin.ecm" "$TEST_DIR/cue_abs/restored.bin" > /dev/null 2>&1; then
    echo "FAIL: unecm --cue with an absolute output path failed"
    exit 1
fi
# Relative output path with a directory, run from the test directory
if ! (cd "$TEST_DIR" && "$UNECM_BIN" --cue mode1.bin.ecm cue_rel/restored.bin > /dev/null 2>&1); then
    echo "FAIL: unecm --cue with a relative output path failed"
    exit 1
fi
for CUE_SHEET in "$TEST_DIR/cue_abs/restored.bin.cue" "$TEST_DIR/cue_rel/restored.bin.cue"; do
    FILE_LINE=$(head -n 1 "$CUE_SHEET" | tr -d '\r')
    if [ "$FILE_LINE" != 'FILE "restored.bin" BINARY' ]; then
        echo "FAIL: CUE FILE entry is not the bare file name in $CUE_SHEET"
        echo "  Got: $FILE_LINE"
        exit 1
    fi
done
echo "PASS: CUE sheet references the image by file name"

# Test 18: Sectors off the 2352-byte grid. The encoder used to test only 2352-byte offsets from
# the start of the input, so none of these images compressed. Each must now shrink and still
# roundtrip, through both the batch and the streaming encoder.
echo ""
echo "--- Test 18: Sectors behind a prefix, with subchannel data, and without headers ---"
generate_sectors mode1 20 "$TEST_DIR/grid_m1.bin"
generate_sectors mode2f1 20 "$TEST_DIR/grid_m2.bin"
{ printf 'X'; cat "$TEST_DIR/grid_m1.bin"; } > "$TEST_DIR/grid_prefix.bin"
"$PYTHON" - "$TEST_DIR" <<'PY'
import sys
d = sys.argv[1]
m1 = open(d + "/grid_m1.bin", "rb").read()
m2 = open(d + "/grid_m2.bin", "rb").read()
# 2448-byte layout: each raw sector followed by 96 bytes of subchannel data
with open(d + "/grid_sub.bin", "wb") as f:
    for i in range(0, len(m1), 2352):
        f.write(m1[i:i + 2352] + bytes(range(96)))
# MODE2/2336: raw Mode 2 sectors without their 16-byte sync and header
with open(d + "/grid_2336.bin", "wb") as f:
    for i in range(0, len(m2), 2352):
        f.write(m2[i + 16:i + 2352])
PY
for NAME in grid_prefix grid_sub grid_2336; do
    SRC="$TEST_DIR/$NAME.bin"
    SRC_SIZE=$(wc -c < "$SRC" | tr -d ' ')
    SRC_SUM=$(sha256sum "$SRC" | cut -d' ' -f1)
    "$ECM_BIN" "$SRC" "$TEST_DIR/$NAME.ecm" > /dev/null 2>&1
    "$ECM_BIN" - - < "$SRC" > "$TEST_DIR/$NAME.stream.ecm" 2> /dev/null
    for ARCHIVE in "$TEST_DIR/$NAME.ecm" "$TEST_DIR/$NAME.stream.ecm"; do
        ARCHIVE_SIZE=$(wc -c < "$ARCHIVE" | tr -d ' ')
        if [ "$ARCHIVE_SIZE" -ge $((SRC_SIZE * 95 / 100)) ]; then
            echo "FAIL: $ARCHIVE did not compress ($ARCHIVE_SIZE of $SRC_SIZE bytes)"
            exit 1
        fi
        rm -f "$TEST_DIR/$NAME.out"
        "$UNECM_BIN" "$ARCHIVE" "$TEST_DIR/$NAME.out" > /dev/null 2>&1
        if [ "$(sha256sum "$TEST_DIR/$NAME.out" | cut -d' ' -f1)" != "$SRC_SUM" ]; then
            echo "FAIL: $ARCHIVE did not roundtrip"
            exit 1
        fi
    done
done
echo "PASS: Off-grid sectors compress and roundtrip in both encoders"

# Test 19: A header-less stream decodes to 2336-byte sectors and its CUE sheet must say so;
# the same sectors with their literal headers make a 2352-byte image
echo ""
echo "--- Test 19: CUE sheet track mode follows the decoded sector size ---"
"$UNECM_BIN" --cue "$TEST_DIR/spec_2336.ecm" "$TEST_DIR/cue_2336.bin" > /dev/null 2>&1
"$UNECM_BIN" --cue "$TEST_DIR/spec_raw.ecm" "$TEST_DIR/cue_2352.bin" > /dev/null 2>&1
TRACK_2336=$(sed -n 2p "$TEST_DIR/cue_2336.bin.cue" | tr -d '\r')
TRACK_2352=$(sed -n 2p "$TEST_DIR/cue_2352.bin.cue" | tr -d '\r')
if [ "$TRACK_2336" != "  TRACK 01 MODE2/2336" ] || [ "$TRACK_2352" != "  TRACK 01 MODE2/2352" ]; then
    echo "FAIL: CUE track modes do not match the decoded sector sizes"
    echo "  Header-less stream: $TRACK_2336"
    echo "  With headers:       $TRACK_2352"
    exit 1
fi
echo "PASS: CUE track mode matches the sector size"

# Test 20: Options are recognised anywhere, '--' ends them, and --help/--version succeed.
# An option after the file names used to become the output name, silently.
echo ""
echo "--- Test 20: Option parsing ---"
cp "$TEST_DIR/mode1.bin.ecm" "$TEST_DIR/opt.bin.ecm"
(cd "$TEST_DIR" && "$UNECM_BIN" opt.bin.ecm --cue > /dev/null 2>&1)
if [ -e "$TEST_DIR/--cue" ] || [ ! -f "$TEST_DIR/opt.bin.cue" ]; then
    echo "FAIL: unecm did not treat a trailing --cue as an option"
    exit 1
fi
(cd "$TEST_DIR" && "$ECM_BIN" "$MODE1_FILE" opt_v.ecm -v > /dev/null 2>&1)
if [ -e "$TEST_DIR/-v" ] || [ ! -f "$TEST_DIR/opt_v.ecm" ]; then
    echo "FAIL: ecm did not treat a trailing -v as an option"
    exit 1
fi
cp "$MODE1_FILE" "$TEST_DIR/-dash.bin"
(cd "$TEST_DIR" && "$ECM_BIN" -- -dash.bin > /dev/null 2>&1)
if [ ! -f "$TEST_DIR/-dash.bin.ecm" ]; then
    echo "FAIL: '--' did not let a file name start with '-'"
    exit 1
fi
if "$ECM_BIN" --no-such-option "$MODE1_FILE" "$TEST_DIR/unknown.ecm" > /dev/null 2>&1; then
    echo "FAIL: ecm accepted an unknown option"
    exit 1
fi
if "$ECM_BIN" "$MODE1_FILE" "$TEST_DIR/a.ecm" "$TEST_DIR/b.ecm" > /dev/null 2>&1; then
    echo "FAIL: ecm accepted three file names"
    exit 1
fi
if ! "$ECM_BIN" --help 2> /dev/null | grep -q "usage:" ||
    ! "$UNECM_BIN" -h 2> /dev/null | grep -q -- "--mode2-2352"; then
    echo "FAIL: --help did not print usage to stdout"
    exit 1
fi
if ! "$ECM_BIN" --version 2> /dev/null | grep -q "^ecm [0-9]" ||
    ! "$UNECM_BIN" -V 2> /dev/null | grep -q "^unecm [0-9]"; then
    echo "FAIL: --version did not print the version to stdout"
    exit 1
fi
echo "PASS: Options parsed anywhere, '--', --help and --version work"

# Test 21: A decode that fails part-way must not leave a truncated image behind
echo ""
echo "--- Test 21: Failed decodes remove their output ---"
head -c 1000 "$TEST_DIR/mode1.bin.ecm" > "$TEST_DIR/truncated.ecm"
if "$UNECM_BIN" "$TEST_DIR/truncated.ecm" "$TEST_DIR/truncated.bin" > /dev/null 2>&1; then
    echo "FAIL: unecm accepted a truncated archive"
    exit 1
fi
if [ -e "$TEST_DIR/truncated.bin" ]; then
    echo "FAIL: unecm left a truncated image behind"
    exit 1
fi
echo "PASS: Failed decodes remove their output"

echo ""
echo "=== All roundtrip tests passed (21/21) ==="
exit 0
