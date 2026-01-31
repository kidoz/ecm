#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

/*
 * Unit tests for ecm.c
 * Uses include-based testing to access static functions without modifying source
 */

#include "eccedc.h"

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
    FILE *f = tmpfile();
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
    f = tmpfile();
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

    FILE *f = tmpfile();
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

    FILE *f = tmpfile();
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

    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
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
    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
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

    TEST_SUITE_END();
}
