#ifndef TEST_COMMON_H
#define TEST_COMMON_H

/*
 * Shared test framework macros for ECM unit tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eccedc.h"

/* Test counters - must be defined in each test file */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                           \
    do {                                     \
        tests_run++;                         \
        printf("  Testing: %s ... ", #name); \
        fflush(stdout);                      \
    } while (0)

#define PASS()            \
    do {                  \
        tests_passed++;   \
        printf("PASS\n"); \
    } while (0)

#define FAIL(msg)                  \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT_EQ(expected, actual)                                                \
    do {                                                                           \
        if ((expected) != (actual)) {                                              \
            printf("FAIL: expected %d, got %d\n", (int)(expected), (int)(actual)); \
            return;                                                                \
        }                                                                          \
    } while (0)

#define ASSERT_EQ_MSG(expected, actual, msg)                                                 \
    do {                                                                                     \
        if ((expected) != (actual)) {                                                        \
            printf("FAIL: %s (expected %d, got %d)\n", msg, (int)(expected), (int)(actual)); \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#define ASSERT_TRUE(cond)                      \
    do {                                       \
        if (!(cond)) {                         \
            printf("FAIL: condition false\n"); \
            return;                            \
        }                                      \
    } while (0)

#define ASSERT_FALSE(cond)                    \
    do {                                      \
        if ((cond)) {                         \
            printf("FAIL: condition true\n"); \
            return;                           \
        }                                     \
    } while (0)

#define ASSERT_NOT_NULL(ptr)                   \
    do {                                       \
        if ((ptr) == nullptr) {                \
            printf("FAIL: pointer is null\n"); \
            return;                            \
        }                                      \
    } while (0)

#define ASSERT_NULL(ptr)                        \
    do {                                        \
        if ((ptr) != nullptr) {                 \
            printf("FAIL: pointer not null\n"); \
            return;                             \
        }                                       \
    } while (0)

#define ASSERT_MEM_EQ(expected, actual, size)            \
    do {                                                 \
        if (memcmp((expected), (actual), (size)) != 0) { \
            printf("FAIL: memory mismatch\n");           \
            return;                                      \
        }                                                \
    } while (0)

/* Print test suite header */
#define TEST_SUITE_BEGIN(name)          \
    do {                                \
        printf("=== %s ===\n\n", name); \
    } while (0)

/* Print test category */
#define TEST_CATEGORY(name)    \
    do {                       \
        printf("%s:\n", name); \
    } while (0)

/* Print test results */
#define TEST_SUITE_END()                                                            \
    do {                                                                            \
        printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run); \
        return (tests_passed == tests_run) ? 0 : 1;                                 \
    } while (0)

/*
 * Fixture builders shared by the encoder and decoder tests.
 * static inline keeps a file that uses only some of them warning-free under werror.
 */

/* Fill the 12-byte sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
static inline void fixture_sync(uint8_t *sector) {
    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;
}

/*
 * Build a valid raw 2352-byte Mode 2 sector (Form 1 or Form 2) carrying the given BCD MSF
 * address. seed varies the user data between sectors. Requires eccedc_init().
 */
static inline void fixture_mode2_sector(uint8_t *sector, sector_type_t form, const uint8_t msf[3],
                                        uint8_t seed) {
    size_t payload =
        form == SECTOR_TYPE_MODE2_FORM1 ? MODE2_FORM1_DATA_SIZE : MODE2_FORM2_DATA_SIZE;
    size_t user_bytes = payload - MODE2_SUBHEADER_SIZE;
    uint8_t *subheader = sector + OFFSET_MODE2_SUBHEADER;

    memset(sector, 0, SECTOR_SIZE_RAW);
    fixture_sync(sector);
    memcpy(sector + OFFSET_HEADER, msf, MODE1_ADDRESS_SIZE);
    sector[OFFSET_MODE] = 0x02;
    /* Submode byte: 0x08 marks Form 1 data, 0x20 marks Form 2 */
    subheader[2] = form == SECTOR_TYPE_MODE2_FORM1 ? 0x08 : 0x20;
    memcpy(subheader + MODE2_SUBHEADER_SIZE, subheader, MODE2_SUBHEADER_SIZE);
    for (size_t i = 0; i < user_bytes; i++) {
        subheader[2 * MODE2_SUBHEADER_SIZE + i] = (uint8_t)((i * seed + 3) & 0xFF);
    }
    eccedc_generate(sector, form);
}

/* Write a type/count record header exactly as ecm does; count 0 with type 0 is the end marker */
static inline void fixture_write_type_count(FILE *f, unsigned type, unsigned count) {
    count--;
    fputc((int)(((count >= 32) << 7) | ((count & 31) << 2) | type), f);
    count >>= 5;
    while (count) {
        fputc((int)(((count >= 128) << 7) | (count & 127)), f);
        count >>= 7;
    }
}

/*
 * Read a record header the way unecm does. count is the decoded count (on-disk value plus
 * one), so the end marker (0xFFFFFFFF on disk) reads back as 0.
 */
static inline bool fixture_read_type_count(FILE *f, unsigned *type, unsigned *count) {
    int c = fgetc(f);
    int bits = 5;

    if (c == EOF)
        return false;
    *type = (unsigned)(c & 3);
    *count = (unsigned)((c >> 2) & 0x1F);
    while (c & 0x80) {
        c = fgetc(f);
        if (c == EOF)
            return false;
        *count |= ((unsigned)(c & 0x7F)) << bits;
        bits += 7;
    }
    (*count)++;
    return true;
}

#endif /* TEST_COMMON_H */
