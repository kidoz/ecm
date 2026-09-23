#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

/*
 * Unit tests for ecm.c
 * Uses include-based testing to access static functions without modifying source
 */

#include "eccedc.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

/* Rename main() and other conflicting symbols from ecm.c */
#define main   ecm_main
#define banner ecm_banner
#include "../src/ecm.c"
#undef main
#undef banner

/* Include shared test framework */
#include "test_common.h"

/*
 * Test: eccedc_init() initializes and edc_compute works correctly
 */
void test_edc_compute(void) {
    TEST(edc_compute);

    eccedc_init();

    /* Zero data should produce zero EDC */
    uint8_t zeros[16] = {0};
    uint32_t edc = edc_compute(0, zeros, 16);
    ASSERT_EQ(0, edc);

    /* Test with known data pattern */
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    edc = edc_compute(0, data, 4);
    /* EDC should be non-zero for non-zero data */
    ASSERT_TRUE(edc != 0);

    /* Test accumulation: computing in parts should equal computing whole */
    uint8_t data2[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint32_t edc_whole = edc_compute(0, data2, 8);
    uint32_t edc_part1 = edc_compute(0, data2, 4);
    uint32_t edc_part2 = edc_compute(edc_part1, data2 + 4, 4);
    ASSERT_EQ(edc_whole, edc_part2);

    PASS();
}

/*
 * Test: check_type() identifies sector types correctly
 */
void test_check_type_literal(void) {
    TEST(check_type_literal);

    eccedc_init();

    /* Random data should be type 0 (literal) */
    uint8_t random_sector[SECTOR_SIZE_RAW];
    for (int i = 0; i < SECTOR_SIZE_RAW; i++) {
        random_sector[i] = (uint8_t)(i * 7 + 13);
    }

    sector_type_t type = check_type_raw(random_sector);
    ASSERT_EQ(SECTOR_TYPE_LITERAL, type);

    PASS();
}

/*
 * Test: check_type() rejects invalid Mode 1 sync pattern
 */
void test_check_type_invalid_sync(void) {
    TEST(check_type_invalid_sync);

    eccedc_init();

    /* Create sector with wrong sync pattern */
    uint8_t sector[SECTOR_SIZE_RAW] = {0};
    sector[0] = 0x01; /* Should be 0x00 */

    sector_type_t type = check_type_raw(sector);
    /* Should not be detected as Mode 1 */
    ASSERT_TRUE(type != SECTOR_TYPE_MODE1);

    PASS();
}

/*
 * Test: write_type_count() encoding
 */
void test_write_type_count(void) {
    TEST(write_type_count);

    /* Create a temporary file for testing */
    FILE *f = test_tmpfile();
    ASSERT_TRUE(f != nullptr);

    /* Write type 1, count 1 */
    (void)write_type_count(f, 1, 1);

    /* Read back and verify */
    rewind(f);
    int byte = fgetc(f);
    /* count=1, so count-1=0, type=1
     * byte = ((0 >= 32) << 7) | ((0 & 31) << 2) | 1 = 0 | 0 | 1 = 1
     */
    ASSERT_EQ(1, byte);

    /* Should be end of data */
    ASSERT_EQ(EOF, fgetc(f));

    fclose(f);

    /* Test larger count */
    f = test_tmpfile();
    ASSERT_TRUE(f != nullptr);

    /* Write type 2, count 33 (requires continuation byte) */
    (void)write_type_count(f, 2, 33);

    rewind(f);
    /* count=33, count-1=32
     * First byte: ((32 >= 32) << 7) | ((32 & 31) << 2) | 2 = 0x80 | 0 | 2 = 0x82
     * count >>= 5 -> 1
     * Second byte: ((1 >= 128) << 7) | (1 & 127) = 0 | 1 = 1
     */
    ASSERT_EQ(0x82, fgetc(f));
    ASSERT_EQ(0x01, fgetc(f));

    fclose(f);

    PASS();
}

/*
 * Test: Mode 1 sector structure validation
 */
void test_mode1_structure(void) {
    TEST(mode1_structure);

    eccedc_init();

    /* Build a valid Mode 1 sector header structure */
    uint8_t sector[SECTOR_SIZE_RAW] = {0};

    /* Sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode byte */
    sector[OFFSET_MODE] = 0x01;

    /* Reserved area (should be zeros) - already zero */

    /* This won't pass check_type because EDC/ECC aren't computed,
     * but it tests the sync pattern detection */
    sector_type_t type = check_type_raw(sector);

    /* May or may not be type 1 depending on EDC/ECC validation */
    /* The important thing is it doesn't crash */
    ASSERT_TRUE(type >= SECTOR_TYPE_LITERAL && type <= SECTOR_TYPE_MODE2_FORM2);

    PASS();
}

/*
 * Test: Constants are defined correctly
 */
void test_constants(void) {
    TEST(constants);

    ASSERT_EQ(2352, SECTOR_SIZE_RAW);
    ASSERT_EQ(2336, SECTOR_SIZE_MODE2);
    ASSERT_EQ(2048, SECTOR_USER_DATA);
    ASSERT_EQ(0x00, SYNC_BYTE_START);
    ASSERT_EQ(0xFF, SYNC_BYTE_MIDDLE);
    ASSERT_EQ(0x00, SYNC_BYTE_END);

    PASS();
}

/*
 * Test: Valid Mode 1 sector is detected correctly
 */
void test_check_type_valid_mode1(void) {
    TEST(check_type_valid_mode1);

    eccedc_init();

    /* Build a complete valid Mode 1 sector */
    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Address (MSF) */
    sector[OFFSET_HEADER + 0] = 0x00;
    sector[OFFSET_HEADER + 1] = 0x02;
    sector[OFFSET_HEADER + 2] = 0x00;

    /* Mode byte */
    sector[OFFSET_MODE] = 0x01;

    /* User data */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[OFFSET_MODE1_DATA + i] = (uint8_t)(i & 0xFF);
    }

    /* Generate valid ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE1);

    /* Now check_type should detect it as Mode 1 */
    sector_type_t type = check_type_raw(sector);
    ASSERT_EQ(SECTOR_TYPE_MODE1, type);

    PASS();
}

/*
 * Test: Valid Mode 2 Form 1 sector is detected correctly
 */
void test_check_type_valid_mode2_form1(void) {
    TEST(check_type_valid_mode2_form1);

    eccedc_init();

    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Subheader (Form 1) and duplicate */
    sector[0x10] = 0x00;
    sector[0x11] = 0x00;
    sector[0x12] = 0x08; /* Form 1 */
    sector[0x13] = 0x00;
    sector[0x14] = sector[0x10];
    sector[0x15] = sector[0x11];
    sector[0x16] = sector[0x12];
    sector[0x17] = sector[0x13];

    /* User data */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[0x18 + i] = (uint8_t)((i * 7) & 0xFF);
    }

    /* Generate ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);

    sector_type_t type = check_type_raw(sector);
    ASSERT_EQ(SECTOR_TYPE_MODE2_FORM1, type);

    PASS();
}

/*
 * Test: Valid Mode 2 Form 2 sector is detected correctly
 */
void test_check_type_valid_mode2_form2(void) {
    TEST(check_type_valid_mode2_form2);

    eccedc_init();

    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Subheader (Form 2) and duplicate */
    sector[0x10] = 0x01;
    sector[0x11] = 0x00;
    sector[0x12] = 0x20; /* Form 2 bit set */
    sector[0x13] = 0x00;
    sector[0x14] = sector[0x10];
    sector[0x15] = sector[0x11];
    sector[0x16] = sector[0x12];
    sector[0x17] = sector[0x13];

    /* User data */
    for (int i = 0; i < 2324; i++) {
        sector[0x18 + i] = (uint8_t)((i * 11) & 0xFF);
    }

    /* Generate ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM2);

    sector_type_t type = check_type_raw(sector);
    ASSERT_EQ(SECTOR_TYPE_MODE2_FORM2, type);

    PASS();
}

/*
 * Test: Mode 2 sector with wrong mode byte is literal
 */
void test_check_type_mode2_wrong_mode(void) {
    TEST(check_type_mode2_wrong_mode);

    eccedc_init();

    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Valid sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Wrong mode byte for Mode 2 */
    sector[OFFSET_MODE] = 0x03; /* Invalid mode */

    sector_type_t type = check_type_raw(sector);
    ASSERT_EQ(SECTOR_TYPE_LITERAL, type);

    PASS();
}

/*
 * Test: Subheader mismatch detected
 */
void test_check_type_subheader_mismatch(void) {
    TEST(check_type_subheader_mismatch);

    eccedc_init();

    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Valid sync */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Mismatched subheader (bytes 0x10-0x13 != 0x14-0x17) */
    sector[0x10] = 0x00;
    sector[0x11] = 0x00;
    sector[0x12] = 0x08;
    sector[0x13] = 0x00;
    /* Different values in copy */
    sector[0x14] = 0xFF;
    sector[0x15] = 0xFF;
    sector[0x16] = 0xFF;
    sector[0x17] = 0xFF;

    sector_type_t type = check_type_raw(sector);
    /* With mismatched subheader, should not be detected as Mode 2 */
    ASSERT_TRUE(type == SECTOR_TYPE_LITERAL || type == SECTOR_TYPE_MODE1);

    PASS();
}

/*
 * Test: write_type_count handles large counts correctly
 */
void test_write_type_count_large(void) {
    TEST(write_type_count_large);

    FILE *f = test_tmpfile();
    ASSERT_TRUE(f != nullptr);

    /* Test with maximum useful count (close to 0xFFFFFFFF - 1) */
    /* 1000 sectors - requires multiple continuation bytes */
    int result = write_type_count(f, SECTOR_TYPE_MODE1, 1000);
    ASSERT_EQ(0, result);

    /* Verify we can read it back correctly */
    rewind(f);

    /* Decode the type/count */
    int c = fgetc(f);
    unsigned type = (unsigned)(c & 3);
    unsigned num = (unsigned)((c >> 2) & 0x1F);
    int bits = 5;

    while (c & 0x80) {
        c = fgetc(f);
        ASSERT_TRUE(c != EOF);
        num |= ((unsigned)(c & 0x7F)) << bits;
        bits += 7;
    }
    num++; /* Decode adds 1 */

    ASSERT_EQ(SECTOR_TYPE_MODE1, type);
    ASSERT_EQ(1000, num);

    fclose(f);
    PASS();
}

/*
 * Test: write_type_count returns int for error checking
 */
void test_write_type_count_returns_int(void) {
    TEST(write_type_count_returns_int);

    FILE *f = test_tmpfile();
    ASSERT_TRUE(f != nullptr);

    /* Successful writes should return 0 */
    int result = write_type_count(f, SECTOR_TYPE_MODE1, 1);
    ASSERT_EQ(0, result);

    result = write_type_count(f, SECTOR_TYPE_MODE2_FORM1, 100);
    ASSERT_EQ(0, result);

    result = write_type_count(f, SECTOR_TYPE_MODE2_FORM2, 10000);
    ASSERT_EQ(0, result);

    fclose(f);
    PASS();
}

/*
 * Test: Literal runs are coalesced instead of per-byte
 */
void test_literal_run_coalesced(void) {
    TEST(literal_run_coalesced);

    /* Create random literal data */
    uint8_t data[4096];
    for (int i = 0; i < (int)sizeof(data); i++) {
        data[i] = (uint8_t)(i * 13 + 7);
    }

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);

    ASSERT_EQ(1, fwrite(data, sizeof(data), 1, fin));
    rewind(fin);

    /* Encode (regular file mode) */
    ASSERT_EQ(0, ecmify(fin, fout, false));
    rewind(fout);

    /* Skip magic */
    ASSERT_EQ('E', fgetc(fout));
    ASSERT_EQ('C', fgetc(fout));
    ASSERT_EQ('M', fgetc(fout));
    ASSERT_EQ(0x00, fgetc(fout));

    /* Decode first type/count */
    int c = fgetc(fout);
    ASSERT_TRUE(c != EOF);
    unsigned type = (unsigned)(c & 3);
    unsigned num = (unsigned)((c >> 2) & 0x1F);
    int bits = 5;
    while (c & 0x80) {
        c = fgetc(fout);
        ASSERT_TRUE(c != EOF);
        num |= ((unsigned)(c & 0x7F)) << bits;
        bits += 7;
    }
    num++; /* Actual count */

    ASSERT_EQ(SECTOR_TYPE_LITERAL, type);
    ASSERT_EQ((unsigned)sizeof(data), num);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: Mode 1 sector with wrong EDC fails validation
 */
void test_check_type_bad_edc(void) {
    TEST(check_type_bad_edc);

    eccedc_init();

    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Valid sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode 1 */
    sector[OFFSET_MODE] = 0x01;

    /* Generate valid ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE1);

    /* Corrupt the EDC */
    sector[OFFSET_MODE1_EDC] ^= 0xFF;

    /* Should not be detected as Mode 1 */
    sector_type_t type = check_type_raw(sector);
    ASSERT_TRUE(type != SECTOR_TYPE_MODE1);

    PASS();
}

/*
 * Test: Sector with wrong ECC fails validation
 */
void test_check_type_bad_ecc(void) {
    TEST(check_type_bad_ecc);

    eccedc_init();

    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Valid sync and mode */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;
    sector[OFFSET_MODE] = 0x01;

    /* Generate valid ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE1);

    /* Corrupt the ECC P section */
    sector[OFFSET_MODE1_ECC_P] ^= 0xFF;
    sector[OFFSET_MODE1_ECC_P + 1] ^= 0xFF;

    /* Should not be detected as Mode 1 */
    sector_type_t type = check_type_raw(sector);
    ASSERT_TRUE(type != SECTOR_TYPE_MODE1);

    PASS();
}

/*
 * Test: All sector types use raw 2352-byte format.
 * This test verifies the sector size constants are consistent.
 */
void test_sector_size_constants(void) {
    TEST(sector_size_constants);

    /* All sector types use 2352-byte raw sectors */
    ASSERT_EQ_MSG(2352, SECTOR_SIZE_RAW, "Raw sector size should be 2352 bytes");
    ASSERT_EQ_MSG(2336, SECTOR_SIZE_MODE2, "Mode 2 sector size should be 2336 bytes");
    ASSERT_EQ_MSG(2048, SECTOR_USER_DATA, "User data area should be 2048 bytes");

    PASS();
}

/*
 * Test: Encoding large literal data should produce efficient output.
 * With O(n²) scanning, encoding N bytes of literal data creates N type/count
 * records, which is both slow and bloated. The encoder should batch literals.
 */
void test_literal_encoding_batching(void) {
    TEST(literal_encoding_batching);

    eccedc_init();

    /* Create input with 1000 bytes of literal (non-sector) data */
    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write 1000 bytes of random-ish data that won't be detected as sectors */
    for (int i = 0; i < 1000; i++) {
        fputc((i * 7 + 13) & 0xFF, fin);
    }
    rewind(fin);

    /* Encode using streaming mode */
    int result = ecmify_streaming(fin, fout, false);
    ASSERT_EQ(0, result);

    /* Check output size */
    fseek(fout, 0, SEEK_END);
    long outsize = ftell(fout);

    /*
     * Expected efficient output:
     * - 4 bytes magic (ECM\0)
     * - ~3 bytes type/count (type 0, count ~1000)
     * - 1000 bytes literal data
     * - 5 bytes end marker
     * - 4 bytes EDC
     * Total: ~1016 bytes
     *
     * Current buggy output with byte-by-byte processing:
     * Each byte gets its own type/count record, so we'd have
     * way more overhead per byte.
     *
     * Allow some overhead but it should be close to input size + ~20 bytes header/footer.
     */
    long expected_max = 1000 + 50; /* Input + reasonable overhead */
    ASSERT_TRUE(outsize <= expected_max);

    /*
     * Also verify we don't have excessive type/count records.
     * With efficient batching, there should be just 1 literal record.
     * Count the number of type/count bytes before the literal data.
     */
    rewind(fout);
    fgetc(fout); /* E */
    fgetc(fout); /* C */
    fgetc(fout); /* M */
    fgetc(fout); /* \0 */

    /* Read first type/count */
    int c = fgetc(fout);
    int type_count_bytes = 1;
    while (c & 0x80) {
        c = fgetc(fout);
        type_count_bytes++;
    }

    /* With efficient encoding: should be 2-3 bytes for type/count of 1000 literals */
    /* With buggy encoding: would be many more records */
    ASSERT_TRUE(type_count_bytes <= 3);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Expect one record of the given type/count whose payload equals expected[0..len).
 */
static bool expect_record(FILE *f, unsigned type, unsigned count, const uint8_t *expected,
                          size_t len) {
    unsigned got_type, got_count;
    uint8_t buf[SECTOR_SIZE_RAW];

    if (!fixture_read_type_count(f, &got_type, &got_count)) {
        printf("FAIL: truncated record header\n");
        return false;
    }
    if (got_type != type || got_count != count) {
        printf("FAIL: expected record type %u count %u, got type %u count %u\n", type, count,
               got_type, got_count);
        return false;
    }
    if (fread(buf, 1, len, f) != len || memcmp(buf, expected, len) != 0) {
        printf("FAIL: record payload mismatch\n");
        return false;
    }
    return true;
}

/*
 * Expect an archive holding n consecutive raw Mode 2 sectors of one form. Each sector must
 * appear as a 16-byte literal (sync + header) followed by a single type 2/3 record, and the
 * trailing EDC must cover the original raw bytes - the upstream ECM layout.
 */
static bool expect_mode2_archive(FILE *f, const uint8_t *sectors, int n, sector_type_t form) {
    size_t payload =
        form == SECTOR_TYPE_MODE2_FORM1 ? MODE2_FORM1_DATA_SIZE : MODE2_FORM2_DATA_SIZE;
    unsigned type, count;
    uint8_t expected_edc[EDC_SIZE];
    uint8_t got_edc[EDC_SIZE];

    rewind(f);
    if (fgetc(f) != ECM_MAGIC_E || fgetc(f) != ECM_MAGIC_C || fgetc(f) != ECM_MAGIC_M ||
        fgetc(f) != ECM_MAGIC_NULL) {
        printf("FAIL: bad magic\n");
        return false;
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *sector = sectors + (size_t)i * SECTOR_SIZE_RAW;
        if (!expect_record(f, SECTOR_TYPE_LITERAL, SECTOR_SYNC_HEADER_SIZE, sector,
                           SECTOR_SYNC_HEADER_SIZE)) {
            return false;
        }
        if (!expect_record(f, form, 1, sector + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE,
                           payload)) {
            return false;
        }
    }
    if (!fixture_read_type_count(f, &type, &count) || type != SECTOR_TYPE_LITERAL || count != 0) {
        printf("FAIL: missing end marker\n");
        return false;
    }
    edc_write_bytes(edc_compute(0, sectors, (size_t)n * SECTOR_SIZE_RAW), expected_edc);
    if (fread(got_edc, 1, EDC_SIZE, f) != EDC_SIZE ||
        memcmp(got_edc, expected_edc, EDC_SIZE) != 0) {
        printf("FAIL: trailing EDC does not cover the raw input\n");
        return false;
    }
    if (fgetc(f) != EOF) {
        printf("FAIL: trailing bytes after EDC\n");
        return false;
    }
    return true;
}

/*
 * Test: batch mode keeps a Mode 2 header with a non-sequential address as literal bytes.
 * Before the fix the 16 header bytes were dropped, so 00:03:00 came back as 00:02:00.
 */
void test_mode2_header_kept_as_literal_batch(void) {
    TEST(mode2_header_kept_as_literal_batch);

    eccedc_init();

    static const uint8_t msf[3] = {0x00, 0x03, 0x00};
    uint8_t sector[SECTOR_SIZE_RAW];
    fixture_mode2_sector(sector, SECTOR_TYPE_MODE2_FORM1, msf, 7);

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);
    ASSERT_EQ(1, fwrite(sector, SECTOR_SIZE_RAW, 1, fin));
    rewind(fin);

    ASSERT_EQ(0, ecmify(fin, fout, false));
    ASSERT_TRUE(expect_mode2_archive(fout, sector, 1, SECTOR_TYPE_MODE2_FORM1));

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: streaming mode produces the same literal + record layout, here for Form 2.
 */
void test_mode2_header_kept_as_literal_streaming(void) {
    TEST(mode2_header_kept_as_literal_streaming);

    eccedc_init();

    static const uint8_t msf[3] = {0x12, 0x34, 0x56};
    uint8_t sector[SECTOR_SIZE_RAW];
    fixture_mode2_sector(sector, SECTOR_TYPE_MODE2_FORM2, msf, 11);

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);
    ASSERT_EQ(1, fwrite(sector, SECTOR_SIZE_RAW, 1, fin));
    rewind(fin);

    ASSERT_EQ(0, ecmify_streaming(fin, fout, false));
    ASSERT_TRUE(expect_mode2_archive(fout, sector, 1, SECTOR_TYPE_MODE2_FORM2));

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: consecutive Mode 2 sectors alternate literal and sector records instead of being
 * merged into one run, because each header must stay in front of its own body.
 */
void test_mode2_run_alternates_literal_and_record(void) {
    TEST(mode2_run_alternates_literal_and_record);

    eccedc_init();

    uint8_t sectors[3 * SECTOR_SIZE_RAW];
    for (int i = 0; i < 3; i++) {
        uint8_t msf[3] = {0x00, 0x05, (uint8_t)(0x10 + i)};
        fixture_mode2_sector(sectors + (size_t)i * SECTOR_SIZE_RAW, SECTOR_TYPE_MODE2_FORM1, msf,
                             (uint8_t)(3 + i));
    }

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);
    ASSERT_EQ(1, fwrite(sectors, sizeof(sectors), 1, fin));
    rewind(fin);

    ASSERT_EQ(0, ecmify(fin, fout, false));
    ASSERT_TRUE(expect_mode2_archive(fout, sectors, 3, SECTOR_TYPE_MODE2_FORM1));

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: a read error on the input stream is an error, not end of input.
 * Reading a directory fails with EISDIR on POSIX; before the fix the encoder answered with
 * a valid empty archive and exit status 0.
 */
void test_streaming_rejects_read_error(void) {
    TEST(streaming_rejects_read_error);

    eccedc_init();

    /* A write-only stream fails every read with the error flag set, on every platform */
    const char *path = "test_unreadable_in.bin";
    FILE *fin = fopen(path, "wb");
    ASSERT_TRUE(fin != nullptr);
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fout != nullptr);

    ASSERT_EQ(1, ecmify_streaming(fin, fout, false));
    ASSERT_TRUE(ferror(fin) != 0);

    fclose(fin);
    fclose(fout);
    remove(path);
    PASS();
}

/*
 * Test: stream_set_binary() stops the CRT translating bytes on a text-mode stream.
 * Only Windows opens streams in text mode, but the check holds everywhere.
 */
void test_stream_set_binary(void) {
    TEST(stream_set_binary);

    const char *path = "test_binary_mode.bin";
    FILE *f = fopen(path, "w"); /* text mode where the platform distinguishes */
    ASSERT_TRUE(f != nullptr);
    ASSERT_TRUE(stream_set_binary(f));
    ASSERT_EQ('\n', fputc('\n', f));
    ASSERT_EQ(0x1A, fputc(0x1A, f));
    fclose(f);

    f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    uint8_t buf[4];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    remove(path);

    ASSERT_EQ(2, n);
    ASSERT_EQ('\n', buf[0]);
    ASSERT_EQ(0x1A, buf[1]);
    PASS();
}

/*
 * Test: file_is_same_as_path() recognises the same file through its own path and through a
 * symlink, and rejects a different or missing file.
 */
void test_file_is_same_as_path(void) {
    TEST(file_is_same_as_path);

    const char *path = "test_same_file.bin";
    const char *other = "test_same_file_other.bin";
    const char *link = "test_same_file.lnk";

    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fputc('x', f);
    fclose(f);
    f = fopen(other, "wb");
    ASSERT_TRUE(f != nullptr);
    fputc('y', f);
    fclose(f);

    f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    ASSERT_TRUE(file_is_same_as_path(f, path));
    ASSERT_FALSE(file_is_same_as_path(f, other));
    ASSERT_FALSE(file_is_same_as_path(f, "test_same_file_missing.bin"));
#if !defined(_WIN32) && !defined(_WIN64)
    remove(link);
    ASSERT_EQ(0, symlink(path, link));
    ASSERT_TRUE(file_is_same_as_path(f, link));
    remove(link);
#else
    (void)link;
#endif
    fclose(f);
    remove(path);
    remove(other);

    PASS();
}

/*
 * Encode data with the batch or streaming encoder. Returns the archive positioned just after
 * the magic, or nullptr on failure.
 */
static FILE *encode_buffer(const uint8_t *data, size_t len, bool streaming) {
    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    if (fin == nullptr || fout == nullptr || fwrite(data, 1, len, fin) != len) {
        printf("FAIL: tmpfile\n");
        goto fail;
    }
    rewind(fin);
    if ((streaming ? ecmify_streaming(fin, fout, false) : ecmify(fin, fout, false)) != 0) {
        printf("FAIL: encoder returned an error\n");
        goto fail;
    }
    fclose(fin);
    rewind(fout);
    if (fgetc(fout) != ECM_MAGIC_E || fgetc(fout) != ECM_MAGIC_C || fgetc(fout) != ECM_MAGIC_M ||
        fgetc(fout) != ECM_MAGIC_NULL) {
        printf("FAIL: bad magic\n");
        fclose(fout);
        return nullptr;
    }
    return fout;

fail:
    if (fin)
        fclose(fin);
    if (fout)
        fclose(fout);
    return nullptr;
}

/*
 * Walk an archive's records and require exactly the given (type, count) sequence followed by
 * the end marker. Payloads are skipped by their stored size.
 */
static bool expect_records(FILE *f, const unsigned expected[][2], size_t n) {
    static const long stored_size[4] = {1, MODE1_ADDRESS_SIZE + SECTOR_USER_DATA,
                                        MODE2_FORM1_DATA_SIZE, MODE2_FORM2_DATA_SIZE};
    unsigned type, count;

    for (size_t i = 0; i < n; i++) {
        if (!fixture_read_type_count(f, &type, &count)) {
            printf("FAIL: truncated record header\n");
            return false;
        }
        if (type != expected[i][0] || count != expected[i][1]) {
            printf("FAIL: record %zu: expected type %u count %u, got type %u count %u\n", i,
                   expected[i][0], expected[i][1], type, count);
            return false;
        }
        fseek(f, stored_size[type] * (long)count, SEEK_CUR);
    }
    if (!fixture_read_type_count(f, &type, &count) || type != SECTOR_TYPE_LITERAL || count != 0) {
        printf("FAIL: expected the end marker after %zu records, got type %u count %u\n", n, type,
               count);
        return false;
    }
    return true;
}

/*
 * Test: the sliding-window EDC matches a direct computation at every offset.
 */
void test_edc_window_roll(void) {
    TEST(edc_window_roll);

    eccedc_init();

    enum {
        WINDOW = 100
    };
    uint8_t data[600];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }
    edc_window_t w;
    edc_window_init(&w, WINDOW);

    uint32_t edc = edc_compute(0, data, WINDOW);
    for (size_t i = 0; i + WINDOW < sizeof(data); i++) {
        edc = edc_window_roll(&w, edc, data[i], data[i + WINDOW]);
        ASSERT_EQ(edc_compute(0, data + i + 1, WINDOW), edc);
    }
    PASS();
}

/*
 * Test: a run never grows past RECORD_COUNT_MAX. The format declares counts of 2^31 and above
 * invalid; the encoder used to split runs only near 2^32, so a large ISO produced one literal
 * record of about 4 billion bytes.
 */
void test_record_count_stays_below_2_31(void) {
    TEST(record_count_stays_below_2_31);

    ASSERT_EQ(0x7FFFFFFF, RECORD_COUNT_MAX);

    run_t run = {SECTOR_TYPE_LITERAL, 0, RECORD_COUNT_MAX - SECTOR_SIZE_RAW};
    ASSERT_TRUE(run_can_extend(&run, SECTOR_TYPE_LITERAL, SECTOR_SIZE_RAW));
    ASSERT_FALSE(run_can_extend(&run, SECTOR_TYPE_LITERAL, SECTOR_SIZE_RAW + 1));
    ASSERT_FALSE(run_can_extend(&run, SECTOR_TYPE_MODE1, 1));

    run.count = 0;
    ASSERT_FALSE(run_can_extend(&run, SECTOR_TYPE_LITERAL, 1)); /* an empty run starts afresh */
    PASS();
}

/*
 * Test: a sector straddling the end of the 1 MB analysis buffer is still modelled. With 446
 * sectors the last one crosses the first buffer boundary; the refill used to be skipped there,
 * and the sector was stored as 2352 literal bytes.
 */
void test_sector_across_buffer_end_is_modelled(void) {
    TEST(sector_across_buffer_end_is_modelled);

    eccedc_init();

    enum {
        SECTORS = 446
    };
    static const uint8_t msf[3] = {0x00, 0x02, 0x00};
    uint8_t *image = malloc((size_t)SECTORS * SECTOR_SIZE_RAW);
    ASSERT_NOT_NULL(image);
    fixture_mode1_sector(image, msf, 3);
    for (size_t i = 1; i < SECTORS; i++) {
        memcpy(image + i * SECTOR_SIZE_RAW, image, SECTOR_SIZE_RAW);
    }

    FILE *f = encode_buffer(image, (size_t)SECTORS * SECTOR_SIZE_RAW, false);
    free(image);
    ASSERT_NOT_NULL(f);
    static const unsigned records[][2] = {{SECTOR_TYPE_MODE1, SECTORS}};
    ASSERT_TRUE(expect_records(f, records, 1));
    fclose(f);
    PASS();
}

/*
 * Test: sectors behind an odd-sized prefix are found by both encoders. Detection used to look
 * only at 2352-byte offsets from the start of the input, so one leading byte disabled all
 * compression.
 */
void test_sector_after_prefix_is_found(void) {
    TEST(sector_after_prefix_is_found);

    eccedc_init();

    static const uint8_t msf0[3] = {0x00, 0x02, 0x00};
    static const uint8_t msf1[3] = {0x00, 0x02, 0x01};
    uint8_t data[1 + 2 * SECTOR_SIZE_RAW];
    data[0] = 'X';
    fixture_mode1_sector(data + 1, msf0, 3);
    fixture_mode1_sector(data + 1 + SECTOR_SIZE_RAW, msf1, 5);

    FILE *batch = encode_buffer(data, sizeof(data), false);
    ASSERT_NOT_NULL(batch);
    static const unsigned batch_records[][2] = {{SECTOR_TYPE_LITERAL, 1}, {SECTOR_TYPE_MODE1, 2}};
    ASSERT_TRUE(expect_records(batch, batch_records, 2));
    fclose(batch);

    FILE *stream = encode_buffer(data, sizeof(data), true);
    ASSERT_NOT_NULL(stream);
    static const unsigned stream_records[][2] = {
        {SECTOR_TYPE_LITERAL, 1}, {SECTOR_TYPE_MODE1, 1}, {SECTOR_TYPE_MODE1, 1}};
    ASSERT_TRUE(expect_records(stream, stream_records, 3));
    fclose(stream);
    PASS();
}

/*
 * Test: a MODE2/2336 image, whose sectors lack sync and header, is modelled record by record as
 * the original encoder did, rather than stored as literal bytes.
 */
void test_headerless_mode2_image_is_modelled(void) {
    TEST(headerless_mode2_image_is_modelled);

    eccedc_init();

    static const uint8_t msf[3] = {0x00, 0x02, 0x00};
    static const sector_type_t forms[3] = {SECTOR_TYPE_MODE2_FORM1, SECTOR_TYPE_MODE2_FORM1,
                                           SECTOR_TYPE_MODE2_FORM2};
    uint8_t image[3 * SECTOR_SIZE_MODE2];
    for (size_t i = 0; i < 3; i++) {
        uint8_t sector[SECTOR_SIZE_RAW];
        fixture_mode2_sector(sector, forms[i], msf, (uint8_t)(7 + i));
        memcpy(image + i * SECTOR_SIZE_MODE2, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
    }

    FILE *batch = encode_buffer(image, sizeof(image), false);
    ASSERT_NOT_NULL(batch);
    static const unsigned batch_records[][2] = {{SECTOR_TYPE_MODE2_FORM1, 2},
                                                {SECTOR_TYPE_MODE2_FORM2, 1}};
    ASSERT_TRUE(expect_records(batch, batch_records, 2));
    fclose(batch);

    FILE *stream = encode_buffer(image, sizeof(image), true);
    ASSERT_NOT_NULL(stream);
    static const unsigned stream_records[][2] = {
        {SECTOR_TYPE_MODE2_FORM1, 1}, {SECTOR_TYPE_MODE2_FORM1, 1}, {SECTOR_TYPE_MODE2_FORM2, 1}};
    ASSERT_TRUE(expect_records(stream, stream_records, 3));
    fclose(stream);
    PASS();
}

/*
 * Test: raw sectors each followed by 96 bytes of subchannel data (2448-byte layout) are all
 * found; only the subchannel bytes stay literal.
 */
void test_sectors_with_subchannel_are_found(void) {
    TEST(sectors_with_subchannel_are_found);

    eccedc_init();

    enum {
        SUBCHANNEL = 96
    };
    static const uint8_t msf0[3] = {0x00, 0x02, 0x00};
    static const uint8_t msf1[3] = {0x00, 0x02, 0x01};
    uint8_t data[2 * (SECTOR_SIZE_RAW + SUBCHANNEL)];
    memset(data, 0x5A, sizeof(data));
    fixture_mode1_sector(data, msf0, 3);
    fixture_mode1_sector(data + SECTOR_SIZE_RAW + SUBCHANNEL, msf1, 5);

    FILE *f = encode_buffer(data, sizeof(data), false);
    ASSERT_NOT_NULL(f);
    static const unsigned records[][2] = {{SECTOR_TYPE_MODE1, 1},
                                          {SECTOR_TYPE_LITERAL, SUBCHANNEL},
                                          {SECTOR_TYPE_MODE1, 1},
                                          {SECTOR_TYPE_LITERAL, SUBCHANNEL}};
    ASSERT_TRUE(expect_records(f, records, 4));
    fclose(f);
    PASS();
}

/*
 * Test: after repetitive padding, where the subheader test passes at nearly every byte and the
 * window EDCs are rolled rather than recomputed, a Mode 2 body that follows is still found at
 * its exact offset.
 */
void test_body_found_after_repetitive_padding(void) {
    TEST(body_found_after_repetitive_padding);

    eccedc_init();
    body_windows_init();

    enum {
        PADDING = 700
    };
    static const uint8_t msf[3] = {0x00, 0x02, 0x00};
    uint8_t sector[SECTOR_SIZE_RAW];
    uint8_t buf[PADDING + SECTOR_SIZE_MODE2];
    fixture_mode2_sector(sector, SECTOR_TYPE_MODE2_FORM1, msf, 9);
    memset(buf, 0xFF, PADDING);
    memcpy(buf + PADDING, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);

    body_edc_t scan = {nullptr, 0, 0};
    scan_t next = scan_next(&scan, buf, sizeof(buf), true);
    ASSERT_EQ(PADDING, next.literal);
    ASSERT_EQ(SECTOR_TYPE_MODE2_FORM1, next.type);
    ASSERT_FALSE(next.raw);

    /* Without end of input, fewer than SECTOR_SIZE_RAW bytes of lookahead decide nothing */
    scan = (body_edc_t){nullptr, 0, 0};
    next = scan_next(&scan, buf + PADDING, SECTOR_SIZE_MODE2, false);
    ASSERT_EQ(0, next.literal);
    ASSERT_EQ(SECTOR_TYPE_LITERAL, next.type);
    PASS();
}

/*
 * Main test runner
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    TEST_SUITE_BEGIN("ECM Unit Tests");

    TEST_CATEGORY("Constants Tests");
    test_constants();

    TEST_CATEGORY("\nEDC Computation Tests");
    test_edc_compute();

    TEST_CATEGORY("\nSector Type Detection Tests");
    test_check_type_literal();
    test_check_type_invalid_sync();
    test_check_type_valid_mode1();
    test_check_type_valid_mode2_form1();
    test_check_type_valid_mode2_form2();
    test_check_type_mode2_wrong_mode();
    test_check_type_subheader_mismatch();
    test_check_type_bad_edc();
    test_check_type_bad_ecc();

    TEST_CATEGORY("\nType/Count Encoding Tests");
    test_write_type_count();
    test_write_type_count_large();
    test_write_type_count_returns_int();
    test_literal_run_coalesced();

    TEST_CATEGORY("\nSector Structure Tests");
    test_mode1_structure();

    TEST_CATEGORY("\nEncoder Efficiency Tests");
    test_sector_size_constants();
    test_literal_encoding_batching();

    TEST_CATEGORY("\nRecord Layout Tests");
    test_mode2_header_kept_as_literal_batch();
    test_mode2_header_kept_as_literal_streaming();
    test_mode2_run_alternates_literal_and_record();

    TEST_CATEGORY("\nSector Scanning Tests");
    test_edc_window_roll();
    test_record_count_stays_below_2_31();
    test_sector_across_buffer_end_is_modelled();
    test_sector_after_prefix_is_found();
    test_headerless_mode2_image_is_modelled();
    test_sectors_with_subchannel_are_found();
    test_body_found_after_repetitive_padding();

    TEST_CATEGORY("\nI/O Safety Tests");
    test_streaming_rejects_read_error();
    test_file_is_same_as_path();
    test_stream_set_binary();

    TEST_SUITE_END();
}
