#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

/*
 * Unit tests for unecm.c
 * Uses include-based testing to access static functions without modifying source
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* Include the shared library header */
#include "eccedc.h"

/* Rename main() and other conflicting symbols from unecm.c */
#define main unecm_main
#define banner unecm_banner
#include "../src/unecm.c"
#undef main
#undef banner

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  Testing: %s ... ", #name); \
        fflush(stdout); \
    } while(0)

#define PASS() \
    do { \
        tests_passed++; \
        printf("PASS\n"); \
    } while(0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
    } while(0)

#define ASSERT_EQ(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("FAIL: expected 0x%X, got 0x%X\n", (unsigned)(expected), (unsigned)(actual)); \
            return; \
        } \
    } while(0)

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: condition false\n"); \
            return; \
        } \
    } while(0)

#define ASSERT_MEM_EQ(expected, actual, size) \
    do { \
        if (memcmp((expected), (actual), (size)) != 0) { \
            printf("FAIL: memory mismatch\n"); \
            return; \
        } \
    } while(0)

/*
 * Test: edc_compute() computes EDC correctly
 */
void test_edc_compute(void) {
    TEST(edc_compute);

    eccedc_init();

    /* Zero data should produce zero EDC */
    uint8_t zeros[16] = {0};
    uint32_t edc = edc_compute(0, zeros, 16);
    ASSERT_EQ(0, edc);

    /* Accumulation test */
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint32_t edc_whole = edc_compute(0, data, 8);
    uint32_t edc_part1 = edc_compute(0, data, 4);
    uint32_t edc_part2 = edc_compute(edc_part1, data + 4, 4);
    ASSERT_EQ(edc_whole, edc_part2);

    PASS();
}

/*
 * Test: edc_compute_block() outputs correct byte format
 */
void test_edc_compute_block(void) {
    TEST(edc_compute_block);

    eccedc_init();

    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t dest[4] = {0};

    edc_compute_block(data, 4, dest);

    /* Verify the EDC is stored in little-endian format */
    uint32_t expected_edc = edc_compute(0, data, 4);
    ASSERT_EQ((expected_edc >> 0) & 0xFF, dest[0]);
    ASSERT_EQ((expected_edc >> 8) & 0xFF, dest[1]);
    ASSERT_EQ((expected_edc >> 16) & 0xFF, dest[2]);
    ASSERT_EQ((expected_edc >> 24) & 0xFF, dest[3]);

    PASS();
}

/*
 * Test: eccedc_generate() for Mode 1 sector
 */
void test_eccedc_generate_mode1(void) {
    TEST(eccedc_generate_mode1);

    eccedc_init();

    /* Create a Mode 1 sector with known data */
    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++) sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Address (MSF) */
    sector[OFFSET_HEADER + 0] = 0x00;  /* Minutes */
    sector[OFFSET_HEADER + 1] = 0x02;  /* Seconds */
    sector[OFFSET_HEADER + 2] = 0x00;  /* Frames */

    /* Mode */
    sector[OFFSET_MODE] = 0x01;

    /* User data (2048 bytes starting at 0x10) */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[OFFSET_MODE1_DATA + i] = (uint8_t)(i & 0xFF);
    }

    /* Generate ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE1);

    /* Verify EDC was written at offset 0x810 (4 bytes) */
    /* EDC should be computed over 0x000 - 0x80F (2064 bytes) */
    uint32_t expected_edc = edc_compute(0, sector, 0x810);
    ASSERT_EQ((expected_edc >> 0) & 0xFF, sector[OFFSET_MODE1_EDC]);
    ASSERT_EQ((expected_edc >> 8) & 0xFF, sector[OFFSET_MODE1_EDC + 1]);
    ASSERT_EQ((expected_edc >> 16) & 0xFF, sector[OFFSET_MODE1_EDC + 2]);
    ASSERT_EQ((expected_edc >> 24) & 0xFF, sector[OFFSET_MODE1_EDC + 3]);

    /* Verify reserved area at 0x814-0x81B is zeroed */
    for (int i = 0; i < RESERVED_SIZE; i++) {
        ASSERT_EQ(0, sector[OFFSET_MODE1_RESERVED + i]);
    }

    /* ECC P is at 0x81C (172 bytes) and ECC Q at 0x8C8 (104 bytes) */
    /* Just verify they're non-zero (proper ECC generation) */
    int ecc_nonzero = 0;
    for (int i = OFFSET_MODE1_ECC_P; i < SECTOR_SIZE_RAW; i++) {
        if (sector[i] != 0) ecc_nonzero = 1;
    }
    ASSERT_TRUE(ecc_nonzero);

    PASS();
}

/*
 * Test: eccedc_generate() for Mode 2 Form 1 sector
 */
void test_eccedc_generate_mode2_form1(void) {
    TEST(eccedc_generate_mode2_form1);

    eccedc_init();

    /* Create a Mode 2 Form 1 sector */
    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++) sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Address */
    sector[OFFSET_HEADER + 0] = 0x00;
    sector[OFFSET_HEADER + 1] = 0x02;
    sector[OFFSET_HEADER + 2] = 0x00;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Subheader (8 bytes: 4 bytes + 4 bytes copy) */
    sector[0x10] = 0x00;  /* File number */
    sector[0x11] = 0x00;  /* Channel */
    sector[0x12] = 0x08;  /* Submode (Form 1) */
    sector[0x13] = 0x00;  /* Coding info */
    /* Copy of subheader */
    sector[0x14] = sector[0x10];
    sector[0x15] = sector[0x11];
    sector[0x16] = sector[0x12];
    sector[0x17] = sector[0x13];

    /* User data (2048 bytes starting at 0x18) */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[0x18 + i] = (uint8_t)((i * 3) & 0xFF);
    }

    /* Generate ECC/EDC for Mode 2 Form 1 */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);

    /* EDC is at 0x818 for Mode 2 Form 1 */
    /* Computed over 0x10-0x817 (2056 bytes) */
    uint32_t expected_edc = edc_compute(0, sector + 0x10, 0x808);
    ASSERT_EQ((expected_edc >> 0) & 0xFF, sector[OFFSET_MODE2_FORM1_EDC]);
    ASSERT_EQ((expected_edc >> 8) & 0xFF, sector[OFFSET_MODE2_FORM1_EDC + 1]);
    ASSERT_EQ((expected_edc >> 16) & 0xFF, sector[OFFSET_MODE2_FORM1_EDC + 2]);
    ASSERT_EQ((expected_edc >> 24) & 0xFF, sector[OFFSET_MODE2_FORM1_EDC + 3]);

    PASS();
}

/*
 * Test: eccedc_generate() for Mode 2 Form 2 sector
 */
void test_eccedc_generate_mode2_form2(void) {
    TEST(eccedc_generate_mode2_form2);

    eccedc_init();

    /* Create a Mode 2 Form 2 sector */
    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    /* Sync pattern */
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++) sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Subheader for Form 2 */
    sector[0x10] = 0x00;
    sector[0x11] = 0x00;
    sector[0x12] = 0x20;  /* Form 2 submode */
    sector[0x13] = 0x00;
    sector[0x14] = sector[0x10];
    sector[0x15] = sector[0x11];
    sector[0x16] = sector[0x12];
    sector[0x17] = sector[0x13];

    /* User data (2324 bytes for Form 2) */
    for (int i = 0; i < 2324; i++) {
        sector[0x18 + i] = (uint8_t)((i * 5) & 0xFF);
    }

    /* Generate EDC for Mode 2 Form 2 */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM2);

    /* EDC is at 0x92C for Mode 2 Form 2 */
    /* Computed over 0x10-0x92B (2332 bytes = 0x91C) */
    uint32_t expected_edc = edc_compute(0, sector + 0x10, 0x91C);
    ASSERT_EQ((expected_edc >> 0) & 0xFF, sector[OFFSET_MODE2_FORM2_EDC]);
    ASSERT_EQ((expected_edc >> 8) & 0xFF, sector[OFFSET_MODE2_FORM2_EDC + 1]);
    ASSERT_EQ((expected_edc >> 16) & 0xFF, sector[OFFSET_MODE2_FORM2_EDC + 2]);
    ASSERT_EQ((expected_edc >> 24) & 0xFF, sector[OFFSET_MODE2_FORM2_EDC + 3]);

    PASS();
}

/*
 * Test: write_cue_file() appends .cue and selects Mode 1 when no Mode 2 sectors are seen
 */
void test_write_cue_file_mode1(void) {
    TEST(write_cue_file_mode1);

    decode_stats_t stats = {1, 0};
    const char *outname = "test_output_mode1.bin";

    ASSERT_EQ(0, write_cue_file(outname, &stats));

    FILE *cue = fopen("test_output_mode1.bin.cue", "r");
    ASSERT_TRUE(cue != NULL);

    char line[128];
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(strstr(line, "MODE1/2352") != NULL);

    fclose(cue);
    remove("test_output_mode1.bin.cue");

    PASS();
}

/*
 * Test: write_cue_file() selects Mode 2 when Mode 2 sectors are present
 */
void test_write_cue_file_mode2(void) {
    TEST(write_cue_file_mode2);

    decode_stats_t stats = {0, 1};
    const char *outname = "test_output_mode2.bin";

    ASSERT_EQ(0, write_cue_file(outname, &stats));

    FILE *cue = fopen("test_output_mode2.bin.cue", "r");
    ASSERT_TRUE(cue != NULL);

    char line[128];
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(strstr(line, "MODE2/2352") != NULL);

    fclose(cue);
    remove("test_output_mode2.bin.cue");

    PASS();
}

/*
 * Test: ECC generation produces consistent results
 */
void test_ecc_consistency(void) {
    TEST(ecc_consistency);

    eccedc_init();

    /* Create two identical sectors */
    uint8_t sector1[SECTOR_SIZE_RAW], sector2[SECTOR_SIZE_RAW];
    memset(sector1, 0, sizeof(sector1));
    memset(sector2, 0, sizeof(sector2));

    /* Set up identical sync and data */
    for (int i = 0; i < SECTOR_SIZE_RAW; i++) {
        sector1[i] = sector2[i] = (uint8_t)(i & 0xFF);
    }

    /* Sync pattern */
    sector1[0] = sector2[0] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++) {
        sector1[i] = sector2[i] = SYNC_BYTE_MIDDLE;
    }
    sector1[0x0B] = sector2[0x0B] = SYNC_BYTE_END;
    sector1[OFFSET_MODE] = sector2[OFFSET_MODE] = 0x01;

    /* Generate ECC/EDC for both */
    eccedc_generate(sector1, SECTOR_TYPE_MODE1);
    eccedc_generate(sector2, SECTOR_TYPE_MODE1);

    /* Results should be identical */
    ASSERT_MEM_EQ(sector1, sector2, SECTOR_SIZE_RAW);

    PASS();
}

/*
 * Test: Constants defined correctly
 */
void test_constants(void) {
    TEST(constants);

    ASSERT_EQ(2352, SECTOR_SIZE_RAW);
    ASSERT_EQ(2336, SECTOR_SIZE_MODE2);
    ASSERT_EQ(4, EDC_SIZE);
    ASSERT_EQ(8, RESERVED_SIZE);
    ASSERT_EQ(172, ECC_P_SIZE);
    ASSERT_EQ(104, ECC_Q_SIZE);

    PASS();
}

/*
 * Test: Magic constants are correct
 */
void test_magic_constants(void) {
    TEST(magic_constants);

    ASSERT_EQ('E', ECM_MAGIC_E);
    ASSERT_EQ('C', ECM_MAGIC_C);
    ASSERT_EQ('M', ECM_MAGIC_M);
    ASSERT_EQ(0x00, ECM_MAGIC_NULL);

    PASS();
}

/*
 * Test: unecmify rejects invalid magic header
 */
void test_unecmify_bad_magic(void) {
    TEST(unecmify_bad_magic);

    eccedc_init();

    /* Create ECM file with wrong magic */
    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != NULL);
    ASSERT_TRUE(fout != NULL);

    /* Write wrong magic header */
    fputc('X', fin);
    fputc('C', fin);
    fputc('M', fin);
    fputc(0x00, fin);

    rewind(fin);

    /* Should fail with wrong magic */
    int result = unecmify(fin, fout, NULL);
    ASSERT_TRUE(result != 0);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: unecmify rejects truncated header
 */
void test_unecmify_truncated_header(void) {
    TEST(unecmify_truncated_header);

    eccedc_init();

    /* Create ECM file with truncated header */
    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != NULL);
    ASSERT_TRUE(fout != NULL);

    /* Write only 2 bytes of magic */
    fputc('E', fin);
    fputc('C', fin);

    rewind(fin);

    /* Should fail with truncated header */
    int result = unecmify(fin, fout, NULL);
    ASSERT_TRUE(result != 0);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: unecmify rejects bad EDC checksum
 */
void test_unecmify_bad_checksum(void) {
    TEST(unecmify_bad_checksum);

    eccedc_init();

    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != NULL);
    ASSERT_TRUE(fout != NULL);

    /* Write valid magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write type 0 (literal), count 4 (3+1) */
    /* type=0, count-1=3: byte = ((3 >= 32) << 7) | ((3 & 31) << 2) | 0 = 0x0C */
    fputc(0x0C, fin);

    /* Write 4 literal bytes */
    fputc(0x01, fin);
    fputc(0x02, fin);
    fputc(0x03, fin);
    fputc(0x04, fin);

    /* Write end marker (type 0, count 0) -> 0xFC = type 0, count-1 = 0x3F means... */
    /* Actually end marker is when decoded count+1 == 0, so encoded count-1 = 0xFFFFFFFF */
    /* This is signaled by special encoding - let me check the code... */
    /* End marker: num == 0xFFFFFFFF after decoding means break */
    /* To encode 0xFFFFFFFF: count-- makes it 0xFFFFFFFE, then encode */
    /* Actually let me just use 0x00 type with count encoding for max */

    /* End-of-records: type 0, count 0 -> 0xFC encodes count-1=63 in first byte? No... */
    /* write_type_count(out, 0, 0) -> count-- = -1 = 0xFFFFFFFF */
    /* fputc(((0xFFFFFFFF >= 32) << 7) | ((0xFFFFFFFF & 31) << 2) | 0 = 0x80 | 0x7C | 0 = 0xFC */
    /* Then count >>= 5 -> lots of continuation bytes... */

    /* Let's just write a minimal end marker manually */
    /* type=0, count-1=0xFFFFFFFF means count=0 in the write_type_count sense */
    /* First byte: 0x80 | (0x1F << 2) | 0 = 0xFC */
    fputc(0xFC, fin);
    /* Need more bytes to encode 0xFFFFFFFF >> 5 = 0x07FFFFFF */
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0x7F, fin);

    /* Write wrong EDC checksum */
    fputc(0x00, fin);
    fputc(0x00, fin);
    fputc(0x00, fin);
    fputc(0x00, fin);

    rewind(fin);

    /* Should fail due to wrong checksum */
    int result = unecmify(fin, fout, NULL);
    ASSERT_TRUE(result != 0);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: unecmify handles empty input (just header + end marker + EDC)
 */
void test_unecmify_empty_data(void) {
    TEST(unecmify_empty_data);

    eccedc_init();

    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != NULL);
    ASSERT_TRUE(fout != NULL);

    /* Write valid magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write end-of-records marker (special encoding for count=0) */
    /* write_type_count(out, 0, 0): count-- wraps to 0xFFFFFFFF */
    fputc(0xFC, fin);  /* type=0, first 5 bits of count-1, continuation */
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0x7F, fin);  /* final byte, no continuation */

    /* Write correct EDC for empty data (0) */
    fputc(0x00, fin);
    fputc(0x00, fin);
    fputc(0x00, fin);
    fputc(0x00, fin);

    rewind(fin);

    int result = unecmify(fin, fout, NULL);
    ASSERT_EQ(0, result);

    /* Output should be empty */
    fseek(fout, 0, SEEK_END);
    long size = ftell(fout);
    ASSERT_EQ(0, size);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: unecmify detects truncated type/count
 */
void test_unecmify_truncated_type_count(void) {
    TEST(unecmify_truncated_type_count);

    eccedc_init();

    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != NULL);
    ASSERT_TRUE(fout != NULL);

    /* Write valid magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write type/count that requires continuation but EOF before it */
    fputc(0x80, fin);  /* Continuation bit set, needs more bytes */
    /* EOF - no more bytes */

    rewind(fin);

    int result = unecmify(fin, fout, NULL);
    ASSERT_TRUE(result != 0);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: TYPE_COUNT_MAX_BITS constant
 */
void test_type_count_max_bits(void) {
    TEST(type_count_max_bits);

    ASSERT_EQ(32, TYPE_COUNT_MAX_BITS);

    PASS();
}

/*
 * Main test runner
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== UNECM Unit Tests ===\n\n");

    printf("Constants Tests:\n");
    test_constants();
    test_magic_constants();
    test_type_count_max_bits();

    printf("\nEDC Computation Tests:\n");
    test_edc_compute();
    test_edc_compute_block();

    printf("\nECC/EDC Generation Tests:\n");
    test_eccedc_generate_mode1();
    test_eccedc_generate_mode2_form1();
    test_eccedc_generate_mode2_form2();
    test_write_cue_file_mode1();
    test_write_cue_file_mode2();

    printf("\nConsistency Tests:\n");
    test_ecc_consistency();

    printf("\nDecoder Error Handling Tests:\n");
    test_unecmify_bad_magic();
    test_unecmify_truncated_header();
    test_unecmify_bad_checksum();
    test_unecmify_empty_data();
    test_unecmify_truncated_type_count();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
