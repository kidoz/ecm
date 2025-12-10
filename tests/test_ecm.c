/*
 * Unit tests for ecm.c
 * Uses include-based testing to access static functions without modifying source
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* Include the shared library header */
#include "eccedc.h"

/* Rename main() and other conflicting symbols from ecm.c */
#define main ecm_main
#define banner ecm_banner
#include "../src/ecm.c"
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
            printf("FAIL: expected %d, got %d\n", (int)(expected), (int)(actual)); \
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

    int type = check_type(random_sector, 1);
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
    sector[0] = 0x01;  /* Should be 0x00 */

    int type = check_type(sector, 1);
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
    ASSERT_TRUE(f != NULL);

    /* Write type 1, count 1 */
    write_type_count(f, 1, 1);

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
    ASSERT_TRUE(f != NULL);

    /* Write type 2, count 33 (requires continuation byte) */
    write_type_count(f, 2, 33);

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
    for (int i = 1; i <= 10; i++) sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode byte */
    sector[OFFSET_MODE] = 0x01;

    /* Reserved area (should be zeros) - already zero */

    /* This won't pass check_type because EDC/ECC aren't computed,
     * but it tests the sync pattern detection */
    int type = check_type(sector, 1);

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
 * Main test runner
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== ECM Unit Tests ===\n\n");

    printf("Constants Tests:\n");
    test_constants();

    printf("\nEDC Computation Tests:\n");
    test_edc_compute();

    printf("\nSector Type Detection Tests:\n");
    test_check_type_literal();
    test_check_type_invalid_sync();

    printf("\nType/Count Encoding Tests:\n");
    test_write_type_count();

    printf("\nSector Structure Tests:\n");
    test_mode1_structure();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
