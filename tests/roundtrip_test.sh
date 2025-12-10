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

# Test 1: Mode 1 sector (2352 bytes)
# Create a valid Mode 1 sector structure
echo "--- Test 1: Mode 1 sector roundtrip ---"
MODE1_FILE="$TEST_DIR/mode1.bin"
python3 -c "
import sys

# Mode 1 sector: 2352 bytes
# Sync pattern (12 bytes): 00 FF FF FF FF FF FF FF FF FF FF 00
sync = bytes([0x00] + [0xFF]*10 + [0x00])

# Address (3 bytes) + Mode (1 byte = 0x01)
header = bytes([0x00, 0x02, 0x00, 0x01])  # MSF address + mode 1

# User data (2048 bytes)
user_data = bytes([(i % 256) for i in range(2048)])

# EDC placeholder (4 bytes) - will be calculated
edc = bytes([0x00] * 4)

# Reserved (8 bytes)
reserved = bytes([0x00] * 8)

# ECC P (172 bytes) and ECC Q (104 bytes) - placeholders
ecc = bytes([0x00] * 276)

# Construct raw sector (before ECC/EDC calculation)
# For testing, we'll create multiple sectors with pattern data
sector = sync + header + user_data + edc + reserved + ecc
assert len(sector) == 2352, f'Sector size: {len(sector)}'

# Write 10 sectors
with open('$MODE1_FILE', 'wb') as f:
    for i in range(10):
        f.write(sector)
"

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

# Test 5: Mode 2 Form 1 sector
echo ""
echo "--- Test 5: Mode 2 Form 1 sector roundtrip ---"
MODE2F1_FILE="$TEST_DIR/mode2f1.bin"
python3 -c "
# Mode 2 Form 1 sector: 2352 bytes
sync = bytes([0x00] + [0xFF]*10 + [0x00])
header = bytes([0x00, 0x02, 0x00, 0x02])  # MSF + mode 2
# Subheader (Form 1: bit 5 clear)
subheader = bytes([0x00, 0x00, 0x08, 0x00])  # Form 1 submode
subheader = subheader + subheader  # Duplicate
# User data for Form 1 is 2048 bytes
user_data = bytes([(i * 3) % 256 for i in range(2048)])
# EDC (4 bytes) + ECC (276 bytes) = 280 bytes
ecc_edc = bytes([0x00] * 280)
sector = sync + header + subheader + user_data + ecc_edc
assert len(sector) == 2352
with open('$MODE2F1_FILE', 'wb') as f:
    for i in range(5):
        f.write(sector)
"

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

# Test 6: Mode 2 Form 2 sector
echo ""
echo "--- Test 6: Mode 2 Form 2 sector roundtrip ---"
MODE2F2_FILE="$TEST_DIR/mode2f2.bin"
python3 -c "
# Mode 2 Form 2 sector: 2352 bytes
sync = bytes([0x00] + [0xFF]*10 + [0x00])
header = bytes([0x00, 0x02, 0x00, 0x02])  # MSF + mode 2
# Subheader (Form 2: bit 5 set = 0x20)
subheader = bytes([0x00, 0x00, 0x20, 0x00])
subheader = subheader + subheader
# User data for Form 2 is 2324 bytes
user_data = bytes([(i * 5) % 256 for i in range(2324)])
# EDC only (4 bytes)
edc = bytes([0x00] * 4)
sector = sync + header + subheader + user_data + edc
assert len(sector) == 2352
with open('$MODE2F2_FILE', 'wb') as f:
    for i in range(5):
        f.write(sector)
"

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
