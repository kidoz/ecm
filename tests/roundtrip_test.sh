#!/bin/bash
# Integration test: roundtrip encoding/decoding
# Tests that: original -> ecm -> unecm -> should equal original

set -e

ECM_BIN="${1:-./build/ecm}"
UNECM_BIN="${2:-./build/unecm}"
TEST_DIR=$(mktemp -d)

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

generate_sectors() {
python3 - <<'PY' "$@"
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
    builders = {
        "mode1": make_mode1,
        "mode2f1": make_mode2_form1,
        "mode2f2": make_mode2_form2,
    }
    build = builders[mode]
    with open(path, "wb") as f:
        for i in range(count):
            f.write(build(msf_from_index(i)))

if __name__ == "__main__":
    main()
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

ORIGINAL_SIZE=$(stat -c%s "$EMPTY_FILE")
DECODED_SIZE=$(stat -c%s "$TEST_DIR/empty_decoded.bin")
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
python3 -c "
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

echo ""
echo "=== All roundtrip tests passed (9/9) ==="
exit 0
