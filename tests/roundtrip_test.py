#!/usr/bin/env python3
"""Integration tests for ECM/UNECM: roundtrips, CLI behavior, and file safety.

Sector and archive fixtures are built by an independent implementation (not the encoder
under test), then the built binaries drive 25 numbered checks. Replaces the former
roundtrip_test.sh, which required bash; this runs natively wherever Python 3 does.

usage: roundtrip_test.py [<ecm-binary> <unecm-binary>]
"""

import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

# Set by main() once the binaries and the work directory are known
ECM_BIN = None
UNECM_BIN = None
TEST_DIR = None

# ----------------------------------------------------------------- fixtures --

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
    return struct.pack("<I", edc_compute(0, data))


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
    ecc_p = ecc_compute_block(sector[ECC_DATA_OFFSET:], ECC_P_MAJOR, ECC_P_MINOR, ECC_P_MULT,
                              ECC_P_INC)
    sector[OFFSET_MODE1_ECC_P:OFFSET_MODE1_ECC_P + len(ecc_p)] = ecc_p
    # ECC Q
    ecc_q = ecc_compute_block(sector[ECC_DATA_OFFSET:], ECC_Q_MAJOR, ECC_Q_MINOR, ECC_Q_MULT,
                              ECC_Q_INC)
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
    minutes = frame // (75 * 60)
    seconds = (frame // 75) % 60
    frames = frame % 75
    # BCD
    return bytes([(minutes // 10 << 4) | (minutes % 10), (seconds // 10 << 4) | (seconds % 10),
                  (frames // 10 << 4) | (frames % 10)])


def generate_sectors(mode, count, path, start=0):
    builders = {"mode1": make_mode1, "mode2f1": make_mode2_form1, "mode2f2": make_mode2_form2}
    build = builders[mode]
    with open(path, "wb") as f:
        for i in range(count):
            f.write(build(msf_from_index(start + i)))


def type_count(rec_type, count):
    c = (count - 1) & 0xFFFFFFFF
    out = bytes([((c >= 32) << 7) | ((c & 31) << 2) | rec_type])
    c >>= 5
    while c:
        out += bytes([((c >= 128) << 7) | (c & 127)])
        c >>= 7
    return out


def build_spec_ecm(layout, raw_path, ecm_path, expected_path=None):
    """Build an ECM stream from a raw Mode 2 image without using the encoder under test.

    with-header: the upstream layout (16-byte literal + type 2/3 record per sector), whose
                 decoded output must equal the raw image
    headerless:  type 2/3 records only, describing 2336-byte sectors; also writes the
                 expected 2336-byte-per-sector output to expected_path
    """
    with open(raw_path, "rb") as f:
        raw = f.read()
    if len(raw) % 2352 != 0:
        raise SystemExit("raw image must be whole 2352-byte sectors")

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
    with open(ecm_path, "wb") as f:
        f.write(ecm)
    if expected_path is not None:
        with open(expected_path, "wb") as f:
            f.write(covered)


# ------------------------------------------------------------------ helpers --

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def random_file(path, size):
    with open(path, "wb") as f:
        f.write(os.urandom(size))


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def section(title):
    print()
    print(f"--- {title} ---")


def encode(src, dst, **kwargs):
    return subprocess.run([ECM_BIN, src, dst], capture_output=True, **kwargs)


def decode(src, dst, **kwargs):
    return subprocess.run([UNECM_BIN, src, dst], capture_output=True, **kwargs)


def check_roundtrip(src, name, print_checksums=False):
    """Encode, decode, and compare; the common shape of the sector-type tests."""
    original = sha256(src)
    if print_checksums:
        print(f"Original file checksum: {original}")
    archive = src + ".ecm"
    decoded = os.path.join(TEST_DIR, f"{name}_decoded.bin")
    encode(src, archive)
    if not os.path.isfile(archive):
        print(f"ERROR: ECM encoding failed for {src}")
        sys.exit(1)
    if print_checksums:
        print(f"Encoded to: {archive}")
    decode(archive, decoded)
    if not os.path.isfile(decoded):
        print(f"ERROR: UNECM decoding failed for {src}")
        sys.exit(1)
    result = sha256(decoded)
    if print_checksums:
        print(f"Decoded file checksum: {result}")
    if original != result:
        fail(f"checksums don't match ({original} != {result})")


def main(argv):
    global ECM_BIN, UNECM_BIN, TEST_DIR

    ECM_BIN = os.path.abspath(argv[1] if len(argv) > 1 else "./build/ecm")
    UNECM_BIN = os.path.abspath(argv[2] if len(argv) > 2 else "./build/unecm")
    TEST_DIR = tempfile.mkdtemp(prefix="ecm-roundtrip-")

    print("=== ECM/UNECM Roundtrip Test ===")
    print(f"ECM binary: {ECM_BIN}")
    print(f"UNECM binary: {UNECM_BIN}")
    print(f"Test directory: {TEST_DIR}")

    for binary, label in ((ECM_BIN, "ecm"), (UNECM_BIN, "unecm")):
        if not os.access(binary, os.X_OK):
            print(f"ERROR: {label} binary not found or not executable: {binary}")
            sys.exit(1)

    # Test 1: Mode 1 sector (2352 bytes) with valid ECC/EDC
    section("Test 1: Mode 1 sector roundtrip")
    MODE1_FILE = os.path.join(TEST_DIR, "mode1.bin")
    generate_sectors("mode1", 10, MODE1_FILE)
    check_roundtrip(MODE1_FILE, "mode1", print_checksums=True)
    print("PASS: Mode 1 roundtrip successful")

    # Test 2: Random data (literal bytes, type 0)
    section("Test 2: Random/literal data roundtrip")
    RANDOM_FILE = os.path.join(TEST_DIR, "random.bin")
    random_file(RANDOM_FILE, 100 * 1024)
    check_roundtrip(RANDOM_FILE, "random", print_checksums=True)
    print("PASS: Random data roundtrip successful")

    # Test 3: Empty file (edge case)
    section("Test 3: Empty file handling")
    EMPTY_FILE = os.path.join(TEST_DIR, "empty.bin")
    open(EMPTY_FILE, "wb").close()
    EMPTY_DECODED = os.path.join(TEST_DIR, "empty_decoded.bin")
    encode(EMPTY_FILE, os.path.join(TEST_DIR, "empty.bin.ecm"))
    if not os.path.isfile(os.path.join(TEST_DIR, "empty.bin.ecm")):
        print("ERROR: ECM encoding failed for empty file")
        sys.exit(1)
    decode(os.path.join(TEST_DIR, "empty.bin.ecm"), EMPTY_DECODED)
    if not os.path.isfile(EMPTY_DECODED):
        print("ERROR: UNECM decoding failed for empty file")
        sys.exit(1)
    if os.path.getsize(EMPTY_FILE) == os.path.getsize(EMPTY_DECODED):
        print("PASS: Empty file roundtrip successful")
    else:
        fail(f"sizes don't match ({os.path.getsize(EMPTY_FILE)} != "
             f"{os.path.getsize(EMPTY_DECODED)})")

    # Test 4: Single byte file
    section("Test 4: Single byte file")
    SINGLE_FILE = os.path.join(TEST_DIR, "single.bin")
    with open(SINGLE_FILE, "wb") as f:
        f.write(b"\x42")
    check_roundtrip(SINGLE_FILE, "single")
    print("PASS: Single byte roundtrip successful")

    # Test 5: Mode 2 Form 1 sector (with valid ECC/EDC)
    section("Test 5: Mode 2 Form 1 sector roundtrip")
    MODE2F1_FILE = os.path.join(TEST_DIR, "mode2f1.bin")
    generate_sectors("mode2f1", 5, MODE2F1_FILE)
    check_roundtrip(MODE2F1_FILE, "mode2f1")
    print("PASS: Mode 2 Form 1 roundtrip successful")

    # Test 6: Mode 2 Form 2 sector (with valid EDC)
    section("Test 6: Mode 2 Form 2 sector roundtrip")
    MODE2F2_FILE = os.path.join(TEST_DIR, "mode2f2.bin")
    generate_sectors("mode2f2", 5, MODE2F2_FILE)
    check_roundtrip(MODE2F2_FILE, "mode2f2")
    print("PASS: Mode 2 Form 2 roundtrip successful")

    # Test 7: Large file (multiple MB)
    section("Test 7: Large file roundtrip")
    LARGE_FILE = os.path.join(TEST_DIR, "large.bin")
    random_file(LARGE_FILE, 5 * 1024 * 1024)
    check_roundtrip(LARGE_FILE, "large")
    print("PASS: Large file roundtrip successful")

    # Test 8: Corrupted ECM file detection
    section("Test 8: Corrupted ECM file detection")
    CORRUPT_FILE = os.path.join(TEST_DIR, "corrupt.bin")
    random_file(CORRUPT_FILE, 10 * 1024)
    CORRUPT_ECM = os.path.join(TEST_DIR, "corrupt.bin.ecm")
    encode(CORRUPT_FILE, CORRUPT_ECM)
    with open(CORRUPT_ECM, "r+b") as f:
        f.seek(-4, os.SEEK_END)
        f.write(b"\xff\xff\xff\xff")
    CORRUPT_DECODED = os.path.join(TEST_DIR, "corrupt_decoded.bin")
    proc = decode(CORRUPT_ECM, CORRUPT_DECODED)
    output = (proc.stdout + proc.stderr).lower()
    if b"edc" in output:
        print("PASS: Corrupted file detected correctly")
    elif not os.path.isfile(CORRUPT_DECODED):
        print("PASS: Corrupted file rejected (no output)")
    elif sha256(CORRUPT_FILE) != sha256(CORRUPT_DECODED):
        print("PASS: Corrupted file detected (checksum mismatch)")
    else:
        fail("corrupted file not detected")
    # An image that failed verification must not be left under the requested name
    if os.path.exists(CORRUPT_DECODED):
        fail("unecm left the unverified output behind")

    # Test 9: Invalid magic header
    section("Test 9: Invalid magic header detection")
    BAD_MAGIC = os.path.join(TEST_DIR, "bad_magic.ecm")
    with open(BAD_MAGIC, "wb") as f:
        f.write(b"XCM\x00")
        f.write(os.urandom(100))
    BAD_MAGIC_DECODED = os.path.join(TEST_DIR, "bad_magic_decoded.bin")
    proc = decode(BAD_MAGIC, BAD_MAGIC_DECODED)
    output = (proc.stdout + proc.stderr).lower()
    if b"header" in output:
        print("PASS: Invalid magic header detected")
    elif not os.path.isfile(BAD_MAGIC_DECODED) or os.path.getsize(BAD_MAGIC_DECODED) == 0:
        print("PASS: Invalid magic header rejected")
    else:
        fail("invalid magic header not detected")

    # Test 10: Mode 2 sectors whose addresses do not start at 00:02:00.
    # The decoder used to regenerate sequential addresses, so this only roundtripped by luck.
    section("Test 10: Mode 2 roundtrip with non-sequential addresses")
    M2ADDR_FILE = os.path.join(TEST_DIR, "mode2_addr.bin")
    generate_sectors("mode2f1", 4, M2ADDR_FILE, 4500)  # first sector at 01:02:00
    M2ADDR_F2 = os.path.join(TEST_DIR, "mode2f2_addr.bin")
    generate_sectors("mode2f2", 3, M2ADDR_F2, 9000)  # 02:02:00
    with open(M2ADDR_FILE, "ab") as out, open(M2ADDR_F2, "rb") as src:
        out.write(src.read())
    check_roundtrip(M2ADDR_FILE, "mode2_addr")
    print("PASS: Mode 2 addresses preserved")

    # Test 11: Mixed image - every sector type plus a partial trailing sector
    section("Test 11: Mixed sector types with partial tail")
    MIXED_FILE = os.path.join(TEST_DIR, "mixed.bin")
    generate_sectors("mode1", 3, MIXED_FILE, 150)
    MIXED_F1 = os.path.join(TEST_DIR, "mixed_f1.bin")
    MIXED_F2 = os.path.join(TEST_DIR, "mixed_f2.bin")
    generate_sectors("mode2f1", 2, MIXED_F1, 153)
    generate_sectors("mode2f2", 2, MIXED_F2, 155)
    with open(MIXED_FILE, "ab") as out, open(MIXED_F1, "rb") as src:
        out.write(src.read())
    with open(MIXED_FILE, "ab") as out, open(MIXED_F2, "rb") as src:
        out.write(src.read())
    with open(MIXED_FILE, "ab") as out:
        out.write(os.urandom(100))
    check_roundtrip(MIXED_FILE, "mixed")
    print("PASS: Mixed image roundtrip successful")

    # Test 12: Streaming mode through stdin/stdout on the mixed image
    section("Test 12: Streaming roundtrip via stdin/stdout")
    MIXED_STREAM = os.path.join(TEST_DIR, "mixed_stream.ecm")
    MIXED_STREAM_DECODED = os.path.join(TEST_DIR, "mixed_stream_decoded.bin")
    with open(MIXED_FILE, "rb") as fin, open(MIXED_STREAM, "wb") as fout:
        subprocess.run([ECM_BIN, "-", "-"], stdin=fin, stdout=fout, stderr=subprocess.DEVNULL)
    with open(MIXED_STREAM, "rb") as fin, open(MIXED_STREAM_DECODED, "wb") as fout:
        subprocess.run([UNECM_BIN, "-", "-"], stdin=fin, stdout=fout, stderr=subprocess.DEVNULL)
    if not os.path.isfile(MIXED_STREAM_DECODED):
        fail("streaming decode produced no output")
    if sha256(MIXED_FILE) == sha256(MIXED_STREAM_DECODED):
        print("PASS: Streaming roundtrip successful")
    else:
        fail("checksums don't match")

    # Test 13: Spec-conformant streams built independently of the encoder.
    # A type 2/3 record must expand to 2336 bytes: with the literal header in front that
    # reproduces the raw image; without it the output is the header-less 2336-byte image.
    section("Test 13: Decoding spec-conformant ECM streams")
    SPEC_RAW_ECM = os.path.join(TEST_DIR, "spec_raw.ecm")
    SPEC_RAW_DECODED = os.path.join(TEST_DIR, "spec_raw_decoded.bin")
    build_spec_ecm("with-header", M2ADDR_FILE, SPEC_RAW_ECM)
    decode(SPEC_RAW_ECM, SPEC_RAW_DECODED)
    if not os.path.isfile(SPEC_RAW_DECODED):
        fail("decoding the with-header spec stream produced no output")
    if sha256(M2ADDR_FILE) != sha256(SPEC_RAW_DECODED):
        fail("literal header + type 2/3 record did not reproduce the raw image "
             f"({sha256(M2ADDR_FILE)} != {sha256(SPEC_RAW_DECODED)})")

    SPEC_2336_ECM = os.path.join(TEST_DIR, "spec_2336.ecm")
    SPEC_2336_EXPECTED = os.path.join(TEST_DIR, "spec_2336_expected.bin")
    SPEC_2336_DECODED = os.path.join(TEST_DIR, "spec_2336_decoded.bin")
    build_spec_ecm("headerless", M2ADDR_FILE, SPEC_2336_ECM, SPEC_2336_EXPECTED)
    decode(SPEC_2336_ECM, SPEC_2336_DECODED)
    if not os.path.isfile(SPEC_2336_DECODED):
        fail("decoding the header-less spec stream produced no output")
    if sha256(SPEC_2336_EXPECTED) != sha256(SPEC_2336_DECODED):
        fail("header-less type 2/3 records did not expand to 2336-byte sectors "
             f"({sha256(SPEC_2336_EXPECTED)} != {sha256(SPEC_2336_DECODED)})")
    print("PASS: Spec-conformant streams decode correctly")

    # Test 14: Output that aliases the input must be refused, leaving the input intact
    section("Test 14: Identical input and output paths are refused")
    SAME_FILE = os.path.join(TEST_DIR, "same.bin")
    shutil.copyfile(MODE1_FILE, SAME_FILE)
    sum_before = sha256(SAME_FILE)
    if encode(SAME_FILE, SAME_FILE).returncode == 0:
        fail("ecm accepted identical input and output paths")
    # os.symlink fails on Windows without symlink privileges; only test a real link
    SAME_ALIAS = os.path.join(TEST_DIR, "same_alias.bin")
    try:
        os.symlink("same.bin", SAME_ALIAS)
    except OSError:
        pass
    if os.path.islink(SAME_ALIAS):
        if encode(SAME_FILE, SAME_ALIAS).returncode == 0:
            fail("ecm accepted a symlink alias of the input as output")
    else:
        print("SKIP: symlinks unavailable, alias check skipped")
    if sha256(SAME_FILE) != sum_before:
        fail("ecm truncated its own input")

    SAME_ECM = os.path.join(TEST_DIR, "same.ecm")
    shutil.copyfile(os.path.join(TEST_DIR, "mode1.bin.ecm"), SAME_ECM)
    sum_before = sha256(SAME_ECM)
    if decode(SAME_ECM, SAME_ECM).returncode == 0:
        fail("unecm accepted identical input and output paths")
    if sha256(SAME_ECM) == sum_before:
        print("PASS: Aliased output refused, inputs intact")
    else:
        fail("unecm truncated its own input")

    # Test 15: A read error on stdin is an error, not an empty input
    section("Test 15: Unreadable stdin is rejected")
    try:
        dir_fd = os.open(TEST_DIR, os.O_RDONLY)
    except OSError:
        print("SKIP: cannot open a directory as a stream on this platform")
    else:
        try:
            # Pass the raw descriptor: wrapping a directory in a file object is refused
            proc = subprocess.run([ECM_BIN, "-", os.path.join(TEST_DIR, "unreadable.ecm")],
                                  stdin=dir_fd, capture_output=True)
        finally:
            os.close(dir_fd)
        if proc.returncode == 0:
            fail("ecm reported success on an unreadable stream")
        print("PASS: Unreadable stdin rejected")

    # Test 16: --mode2-2352 restores an archive written by versions 1.2.0 to 1.3.1.
    # Those versions stored raw Mode 2 sectors as header-less records and regenerated sequential
    # addresses from 00:02:00 on decode, so a header-less stream of an image whose addresses are
    # sequential must come back byte for byte with the option, and as 2336-byte bodies without.
    section("Test 16: Legacy archive restored with --mode2-2352")
    LEGACY_RAW = os.path.join(TEST_DIR, "legacy.bin")
    LEGACY_F2 = os.path.join(TEST_DIR, "legacy_f2.bin")
    generate_sectors("mode2f1", 3, LEGACY_RAW, 0)  # 00:02:00 .. 00:02:02
    generate_sectors("mode2f2", 2, LEGACY_F2, 3)  # 00:02:03 .. 00:02:04
    with open(LEGACY_RAW, "ab") as out, open(LEGACY_F2, "rb") as src:
        out.write(src.read())
    LEGACY_ECM = os.path.join(TEST_DIR, "legacy.ecm")
    build_spec_ecm("headerless", LEGACY_RAW, LEGACY_ECM)

    LEGACY_RESTORED = os.path.join(TEST_DIR, "legacy_restored.bin")
    subprocess.run([UNECM_BIN, "--mode2-2352", LEGACY_ECM, LEGACY_RESTORED],
                   capture_output=True)
    if not os.path.isfile(LEGACY_RESTORED) or sha256(LEGACY_RAW) != sha256(LEGACY_RESTORED):
        fail("--mode2-2352 did not restore the raw image")
    # Options may be combined in any order with the others
    LEGACY_CUE = os.path.join(TEST_DIR, "legacy_cue.bin")
    subprocess.run([UNECM_BIN, "--cue", "--mode2-2352", "-v", LEGACY_ECM, LEGACY_CUE],
                   capture_output=True)
    cue_path = LEGACY_CUE + ".cue"
    if not os.path.isfile(cue_path) or "MODE2/2352" not in open(cue_path).read():
        fail("--mode2-2352 combined with --cue and -v did not produce a MODE2/2352 cue sheet")
    if subprocess.run([UNECM_BIN, "--no-such-option", LEGACY_ECM,
                       os.path.join(TEST_DIR, "legacy_bad.bin")],
                      capture_output=True).returncode == 0:
        fail("unecm accepted an unknown option")
    print("PASS: Legacy archive restored with --mode2-2352")

    # Test 17: The CUE sheet is written next to the image and players resolve its FILE entry
    # relative to the sheet, so the entry must be the bare file name. Writing the output path as
    # typed made a sheet for outdir/restored.bin point at outdir/outdir/restored.bin.
    section("Test 17: CUE sheet references the image by file name")
    CUE_ABS = os.path.join(TEST_DIR, "cue_abs")
    CUE_REL = os.path.join(TEST_DIR, "cue_rel")
    os.makedirs(CUE_ABS, exist_ok=True)
    os.makedirs(CUE_REL, exist_ok=True)
    # Absolute output path (on Windows it also carries a drive letter)
    if subprocess.run([UNECM_BIN, "--cue", os.path.join(TEST_DIR, "mode1.bin.ecm"),
                       os.path.join(CUE_ABS, "restored.bin")],
                      capture_output=True).returncode != 0:
        fail("unecm --cue with an absolute output path failed")
    # Relative output path with a directory, run from the test directory
    if subprocess.run([UNECM_BIN, "--cue", "mode1.bin.ecm", "cue_rel/restored.bin"],
                      cwd=TEST_DIR, capture_output=True).returncode != 0:
        fail("unecm --cue with a relative output path failed")
    for sheet in (os.path.join(CUE_ABS, "restored.bin.cue"),
                  os.path.join(CUE_REL, "restored.bin.cue")):
        line = open(sheet).readline().rstrip("\r\n")
        if line != 'FILE "restored.bin" BINARY':
            fail(f"CUE FILE entry is not the bare file name in {sheet}: {line!r}")
    print("PASS: CUE sheet references the image by file name")

    # Test 18: Sectors off the 2352-byte grid. The encoder used to test only 2352-byte offsets
    # from the start of the input, so none of these images compressed. Each must now shrink and
    # still roundtrip, through both the batch and the streaming encoder.
    section("Test 18: Sectors behind a prefix, with subchannel data, and without headers")
    GRID_M1 = os.path.join(TEST_DIR, "grid_m1.bin")
    GRID_M2 = os.path.join(TEST_DIR, "grid_m2.bin")
    generate_sectors("mode1", 20, GRID_M1)
    generate_sectors("mode2f1", 20, GRID_M2)
    GRID_PREFIX = os.path.join(TEST_DIR, "grid_prefix.bin")
    with open(GRID_PREFIX, "wb") as out, open(GRID_M1, "rb") as src:
        out.write(b"X")
        out.write(src.read())
    m1 = open(GRID_M1, "rb").read()
    m2 = open(GRID_M2, "rb").read()
    # 2448-byte layout: each raw sector followed by 96 bytes of subchannel data
    with open(os.path.join(TEST_DIR, "grid_sub.bin"), "wb") as f:
        for i in range(0, len(m1), 2352):
            f.write(m1[i:i + 2352] + bytes(range(96)))
    # MODE2/2336: raw Mode 2 sectors without their 16-byte sync and header
    with open(os.path.join(TEST_DIR, "grid_2336.bin"), "wb") as f:
        for i in range(0, len(m2), 2352):
            f.write(m2[i + 16:i + 2352])
    for name in ("grid_prefix", "grid_sub", "grid_2336"):
        src_path = os.path.join(TEST_DIR, name + ".bin")
        src_size = os.path.getsize(src_path)
        src_sum = sha256(src_path)
        batch = os.path.join(TEST_DIR, name + ".ecm")
        stream = os.path.join(TEST_DIR, name + ".stream.ecm")
        subprocess.run([ECM_BIN, src_path, batch], capture_output=True)
        with open(src_path, "rb") as fin, open(stream, "wb") as fout:
            subprocess.run([ECM_BIN, "-", "-"], stdin=fin, stdout=fout,
                           stderr=subprocess.DEVNULL)
        for archive in (batch, stream):
            archive_size = os.path.getsize(archive)
            if archive_size >= src_size * 95 // 100:
                fail(f"{archive} did not compress ({archive_size} of {src_size} bytes)")
            out_path = os.path.join(TEST_DIR, name + ".out")
            if os.path.exists(out_path):
                os.remove(out_path)
            subprocess.run([UNECM_BIN, archive, out_path], capture_output=True)
            if not os.path.isfile(out_path) or sha256(out_path) != src_sum:
                fail(f"{archive} did not roundtrip")
    print("PASS: Off-grid sectors compress and roundtrip in both encoders")

    # Test 19: A header-less stream decodes to 2336-byte sectors and its CUE sheet must say so;
    # the same sectors with their literal headers make a 2352-byte image
    section("Test 19: CUE sheet track mode follows the decoded sector size")
    CUE_2336 = os.path.join(TEST_DIR, "cue_2336.bin")
    CUE_2352 = os.path.join(TEST_DIR, "cue_2352.bin")
    subprocess.run([UNECM_BIN, "--cue", SPEC_2336_ECM, CUE_2336], capture_output=True)
    subprocess.run([UNECM_BIN, "--cue", SPEC_RAW_ECM, CUE_2352], capture_output=True)
    if not os.path.isfile(CUE_2336 + ".cue") or not os.path.isfile(CUE_2352 + ".cue"):
        fail("--cue did not produce both sheets")
    track_2336 = open(CUE_2336 + ".cue").read().splitlines()[1].rstrip("\r")
    track_2352 = open(CUE_2352 + ".cue").read().splitlines()[1].rstrip("\r")
    if track_2336 != "  TRACK 01 MODE2/2336" or track_2352 != "  TRACK 01 MODE2/2352":
        fail("CUE track modes do not match the decoded sector sizes "
             f"({track_2336!r}, {track_2352!r})")
    print("PASS: CUE track mode matches the sector size")

    # Test 20: Options are recognised anywhere, '--' ends them, and --help/--version succeed.
    # An option after the file names used to become the output name, silently.
    section("Test 20: Option parsing")
    shutil.copyfile(os.path.join(TEST_DIR, "mode1.bin.ecm"),
                    os.path.join(TEST_DIR, "opt.bin.ecm"))
    subprocess.run([UNECM_BIN, "opt.bin.ecm", "--cue"], cwd=TEST_DIR, capture_output=True)
    if os.path.exists(os.path.join(TEST_DIR, "--cue")) or not os.path.isfile(
            os.path.join(TEST_DIR, "opt.bin.cue")):
        fail("unecm did not treat a trailing --cue as an option")
    subprocess.run([ECM_BIN, MODE1_FILE, "opt_v.ecm", "-v"], cwd=TEST_DIR, capture_output=True)
    if os.path.exists(os.path.join(TEST_DIR, "-v")) or not os.path.isfile(
            os.path.join(TEST_DIR, "opt_v.ecm")):
        fail("ecm did not treat a trailing -v as an option")
    shutil.copyfile(MODE1_FILE, os.path.join(TEST_DIR, "-dash.bin"))
    subprocess.run([ECM_BIN, "--", "-dash.bin"], cwd=TEST_DIR, capture_output=True)
    if not os.path.isfile(os.path.join(TEST_DIR, "-dash.bin.ecm")):
        fail("'--' did not let a file name start with '-'")
    if subprocess.run([ECM_BIN, "--no-such-option", MODE1_FILE,
                       os.path.join(TEST_DIR, "unknown.ecm")],
                      capture_output=True).returncode == 0:
        fail("ecm accepted an unknown option")
    if subprocess.run([ECM_BIN, MODE1_FILE, os.path.join(TEST_DIR, "a.ecm"),
                       os.path.join(TEST_DIR, "b.ecm")], capture_output=True).returncode == 0:
        fail("ecm accepted three file names")
    proc = subprocess.run([ECM_BIN, "--help"], capture_output=True)
    if b"usage:" not in proc.stdout:
        fail("--help did not print usage to stdout")
    proc = subprocess.run([UNECM_BIN, "-h"], capture_output=True)
    if b"--mode2-2352" not in proc.stdout:
        fail("-h did not print usage to stdout")
    proc = subprocess.run([ECM_BIN, "--version"], capture_output=True)
    if not re.match(rb"^ecm [0-9]", proc.stdout):
        fail("--version did not print the version to stdout")
    proc = subprocess.run([UNECM_BIN, "-V"], capture_output=True)
    if not re.match(rb"^unecm [0-9]", proc.stdout):
        fail("-V did not print the version to stdout")
    print("PASS: Options parsed anywhere, '--', --help and --version work")

    # Test 21: A decode that fails part-way must not leave a truncated image behind
    section("Test 21: Failed decodes remove their output")
    TRUNCATED = os.path.join(TEST_DIR, "truncated.ecm")
    with open(os.path.join(TEST_DIR, "mode1.bin.ecm"), "rb") as f:
        data = f.read(1000)
    with open(TRUNCATED, "wb") as f:
        f.write(data)
    TRUNCATED_OUT = os.path.join(TEST_DIR, "truncated.bin")
    if decode(TRUNCATED, TRUNCATED_OUT).returncode == 0:
        fail("unecm accepted a truncated archive")
    if os.path.exists(TRUNCATED_OUT):
        fail("unecm left a truncated image behind")
    print("PASS: Failed decodes remove their output")

    # Test 22: An output through a symlink is refused. fopen would truncate the link target, and
    # a failed-run cleanup would remove only the link, leaving partial bytes under another name.
    section("Test 22: Symlinked output paths are refused")
    PRECIOUS = os.path.join(TEST_DIR, "precious.bin")
    with open(PRECIOUS, "wb") as f:
        f.write(b"precious payload")
    LINK_OUT = os.path.join(TEST_DIR, "link_out.bin")
    try:
        os.symlink(PRECIOUS, LINK_OUT)
    except OSError:
        pass
    if os.path.islink(LINK_OUT):
        sum_before = sha256(PRECIOUS)
        if encode(MODE1_FILE, LINK_OUT).returncode == 0:
            fail("ecm accepted a symlinked output")
        if decode(os.path.join(TEST_DIR, "mode1.bin.ecm"), LINK_OUT).returncode == 0:
            fail("unecm accepted a symlinked output")
        if sha256(PRECIOUS) != sum_before:
            fail("a refused symlinked output still modified its target")
        print("PASS: Symlinked outputs refused, targets intact")
    else:
        print("SKIP: symlinks unavailable, symlinked-output check skipped")

    # Test 23: A CUE sheet path naming the input archive or the output image is refused. The
    # sheet is opened with a truncating mode after the decode, so an alias would replace the
    # archive or the freshly decoded image with the sheet. Hard links may be unavailable (exFAT,
    # some network mounts); a check is skipped when its link cannot be made.
    section("Test 23: CUE sheet aliasing input or image is refused")
    alias_checks = 0
    ALIASIN = os.path.join(TEST_DIR, "aliasin.ecm")
    shutil.copyfile(os.path.join(TEST_DIR, "mode1.bin.ecm"), ALIASIN)
    try:
        os.link(ALIASIN, os.path.join(TEST_DIR, "aliasin.cue"))
    except OSError:
        print("SKIP: hard links unavailable, archive-aliasing check skipped")
    else:
        alias_checks += 1
        archive_sum = sha256(ALIASIN)
        # Default output: the sheet is aliasin.cue, the hard link to the archive
        if subprocess.run([UNECM_BIN, "--cue", "aliasin.ecm"], cwd=TEST_DIR,
                          capture_output=True).returncode == 0:
            fail("unecm --cue accepted a sheet path aliasing the input archive")
        if sha256(ALIASIN) != archive_sum:
            fail("the aliased archive was modified")
        if os.path.exists(os.path.join(TEST_DIR, "aliasin")):
            fail("unecm decoded despite an unsafe sheet path")

    ALIASED = os.path.join(TEST_DIR, "aliased.bin")
    with open(ALIASED, "wb") as f:
        f.write(b"precious image bytes")
    try:
        os.link(ALIASED, ALIASED + ".cue")
    except OSError:
        print("SKIP: hard links unavailable, image-aliasing check skipped")
    else:
        alias_checks += 1
        sum_before = sha256(ALIASED)
        if subprocess.run([UNECM_BIN, "--cue", os.path.join(TEST_DIR, "mode1.bin.ecm"), ALIASED],
                          capture_output=True).returncode == 0:
            fail("unecm --cue accepted a sheet path aliasing the output image")
        if sha256(ALIASED) != sum_before:
            fail("the aliased image was modified")

    LINKCUE = os.path.join(TEST_DIR, "linkcue.ecm")
    shutil.copyfile(os.path.join(TEST_DIR, "mode1.bin.ecm"), LINKCUE)
    LINKCUE_CUE = os.path.join(TEST_DIR, "linkcue_img.bin.cue")
    try:
        os.symlink(LINKCUE, LINKCUE_CUE)
    except OSError:
        pass
    if os.path.islink(LINKCUE_CUE):
        alias_checks += 1
        LINKCUE_IMG = os.path.join(TEST_DIR, "linkcue_img.bin")
        if subprocess.run([UNECM_BIN, "--cue", LINKCUE, LINKCUE_IMG],
                          capture_output=True).returncode == 0:
            fail("unecm --cue accepted a symlinked sheet path")
        if os.path.exists(LINKCUE_IMG):
            fail("unecm decoded despite an unsafe sheet path")
    else:
        print("SKIP: symlinks unavailable, symlinked-sheet check skipped")

    if alias_checks > 0:
        print("PASS: CUE sheet aliases refused, archive and image intact")
    else:
        print("SKIP: no link kind available, CUE alias checks skipped")

    # Test 24: --cue misuse is an error. A decode to stdout cannot produce a sheet, and an image
    # name with a quote or newline cannot appear in a CUE FILE line, which has no escaping.
    section("Test 24: --cue misuse is an error")
    if subprocess.run([UNECM_BIN, "--cue", os.path.join(TEST_DIR, "mode1.bin.ecm"), "-"],
                      capture_output=True).returncode == 0:
        fail("unecm --cue silently accepted a stdout output")
    try:
        QUOTE_BIN = os.path.join(TEST_DIR, 'quote"name.bin')
        random_file(QUOTE_BIN, 100000)
    except OSError:
        # Windows file names cannot contain a quote
        print("SKIP: this platform does not allow a quote in a file name")
    else:
        QUOTE_ECM = QUOTE_BIN + ".ecm"
        encode(QUOTE_BIN, QUOTE_ECM)
        QUOTE_OUT = os.path.join(TEST_DIR, 'quote"name.out.bin')
        if subprocess.run([UNECM_BIN, "--cue", QUOTE_ECM, QUOTE_OUT],
                          capture_output=True).returncode == 0:
            fail("unecm --cue accepted an image name with a quote")
        if subprocess.run([UNECM_BIN, QUOTE_ECM, QUOTE_OUT], capture_output=True).returncode != 0:
            fail("unecm rejected a quote-named image without --cue")
    print("PASS: --cue misuse is an error")

    # Test 25: A failed run must not leave partial bytes reachable through another hard link to
    # the output. The requested name is removed and the inode itself is emptied first, so a twin
    # shows an empty file instead of a partially written image.
    section("Test 25: Failed runs empty the inode for hard-linked twins")
    TWIN_SRC = os.path.join(TEST_DIR, "twin_src.bin")
    TWIN_ECM = os.path.join(TEST_DIR, "twin.ecm")
    TWIN_TRUNC = os.path.join(TEST_DIR, "twin_trunc.ecm")
    random_file(TWIN_SRC, 200000)
    encode(TWIN_SRC, TWIN_ECM)
    with open(TWIN_ECM, "rb") as f:
        data = f.read(60000)
    with open(TWIN_TRUNC, "wb") as f:
        f.write(data)
    TWIN_REAL = os.path.join(TEST_DIR, "twin_real.bin")
    with open(TWIN_REAL, "wb") as f:
        f.write(b"precious payload")
    TWIN_OUT = os.path.join(TEST_DIR, "twin_out.bin")
    try:
        os.link(TWIN_REAL, TWIN_OUT)
    except OSError:
        print("SKIP: hard links unavailable, hard-linked-twin check skipped")
    else:
        if decode(TWIN_TRUNC, TWIN_OUT).returncode == 0:
            fail("unecm accepted a truncated archive")
        if os.path.exists(TWIN_OUT):
            fail("unecm left the failed output name behind")
        if os.path.getsize(TWIN_REAL) != 0:
            fail(f"the hard-linked twin kept {os.path.getsize(TWIN_REAL)} partial bytes")
        print("PASS: Failed run emptied the inode, twin keeps nothing")

    print()
    print("=== All roundtrip tests passed (25/25) ===")


if __name__ == "__main__":
    try:
        main(sys.argv)
    finally:
        if TEST_DIR is not None:
            shutil.rmtree(TEST_DIR, ignore_errors=True)
