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

echo ""
echo "=== All roundtrip tests passed ==="
exit 0
