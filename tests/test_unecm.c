#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

/*
 * Unit tests for unecm.c
 * Uses include-based testing to access static functions without modifying source
 */

#include "eccedc.h"

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#define close  _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

/* Rename main() and other conflicting symbols from unecm.c */
#define main   unecm_main
#define banner unecm_banner
#include "../src/unecm.c"
#undef main
#undef banner

/* Include shared test framework */
#include "test_common.h"

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
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Address (MSF) */
    sector[OFFSET_HEADER + 0] = 0x00; /* Minutes */
    sector[OFFSET_HEADER + 1] = 0x02; /* Seconds */
    sector[OFFSET_HEADER + 2] = 0x00; /* Frames */

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
    bool ecc_nonzero = false;
    for (int i = OFFSET_MODE1_ECC_P; i < SECTOR_SIZE_RAW; i++) {
        if (sector[i] != 0)
            ecc_nonzero = true;
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
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Address */
    sector[OFFSET_HEADER + 0] = 0x00;
    sector[OFFSET_HEADER + 1] = 0x02;
    sector[OFFSET_HEADER + 2] = 0x00;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Subheader (8 bytes: 4 bytes + 4 bytes copy) */
    sector[0x10] = 0x00; /* File number */
    sector[0x11] = 0x00; /* Channel */
    sector[0x12] = 0x08; /* Submode (Form 1) */
    sector[0x13] = 0x00; /* Coding info */
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
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;

    /* Mode 2 */
    sector[OFFSET_MODE] = 0x02;

    /* Subheader for Form 2 */
    sector[0x10] = 0x00;
    sector[0x11] = 0x00;
    sector[0x12] = 0x20; /* Form 2 submode */
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

    decode_stats_t stats = {true, false};
    const char *outname = "test_output_mode1.bin";

    ASSERT_EQ(0, write_cue_file(outname, &stats));

    FILE *cue = fopen("test_output_mode1.bin.cue", "r");
    ASSERT_TRUE(cue != nullptr);

    char line[128];
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(strstr(line, "MODE1/2352") != nullptr);

    fclose(cue);
    remove("test_output_mode1.bin.cue");

    PASS();
}

/*
 * Test: write_cue_file() selects Mode 2 when Mode 2 sectors are present
 */
void test_write_cue_file_mode2(void) {
    TEST(write_cue_file_mode2);

    decode_stats_t stats = {false, true};
    const char *outname = "test_output_mode2.bin";

    ASSERT_EQ(0, write_cue_file(outname, &stats));

    FILE *cue = fopen("test_output_mode2.bin.cue", "r");
    ASSERT_TRUE(cue != nullptr);

    char line[128];
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(fgets(line, sizeof(line), cue));
    ASSERT_TRUE(strstr(line, "MODE2/2352") != nullptr);

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
    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write wrong magic header */
    fputc('X', fin);
    fputc('C', fin);
    fputc('M', fin);
    fputc(0x00, fin);

    rewind(fin);

    /* Should fail with wrong magic */
    int result = unecmify(fin, fout, nullptr, false, false, false);
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
    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write only 2 bytes of magic */
    fputc('E', fin);
    fputc('C', fin);

    rewind(fin);

    /* Should fail with truncated header */
    int result = unecmify(fin, fout, nullptr, false, false, false);
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

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write valid magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write type 0 (literal), count 4 (3+1) */
    fputc(0x0C, fin);

    /* Write 4 literal bytes */
    fputc(0x01, fin);
    fputc(0x02, fin);
    fputc(0x03, fin);
    fputc(0x04, fin);

    /* Write end-of-records marker */
    fputc(0xFC, fin);
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
    int result = unecmify(fin, fout, nullptr, false, false, false);
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

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write valid magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write end-of-records marker */
    fputc(0xFC, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0x7F, fin);

    /* Write correct EDC for empty data (0) */
    fputc(0x00, fin);
    fputc(0x00, fin);
    fputc(0x00, fin);
    fputc(0x00, fin);

    rewind(fin);

    int result = unecmify(fin, fout, nullptr, false, false, false);
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

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write valid magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write type/count that requires continuation but EOF before it */
    fputc(0x80, fin);

    rewind(fin);

    int result = unecmify(fin, fout, nullptr, false, false, false);
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
 * Write a complete ECM stream for one raw Mode 2 sector. with_header selects the upstream
 * layout for raw images (a 16-byte literal for sync + header, then the type 2/3 record);
 * without it the stream describes a header-less 2336-byte sector. The trailing EDC covers
 * whatever the stream is expected to reproduce.
 */
static void write_mode2_stream(FILE *f, const uint8_t *sector, sector_type_t form,
                               bool with_header) {
    size_t payload =
        form == SECTOR_TYPE_MODE2_FORM1 ? MODE2_FORM1_DATA_SIZE : MODE2_FORM2_DATA_SIZE;
    const uint8_t *covered = with_header ? sector : sector + OFFSET_MODE2_SUBHEADER;
    size_t covered_len = with_header ? SECTOR_SIZE_RAW : SECTOR_SIZE_MODE2;
    uint8_t edc[EDC_SIZE];

    fputc(ECM_MAGIC_E, f);
    fputc(ECM_MAGIC_C, f);
    fputc(ECM_MAGIC_M, f);
    fputc(ECM_MAGIC_NULL, f);
    if (with_header) {
        fixture_write_type_count(f, SECTOR_TYPE_LITERAL, SECTOR_SYNC_HEADER_SIZE);
        fwrite(sector, 1, SECTOR_SYNC_HEADER_SIZE, f);
    }
    fixture_write_type_count(f, form, 1);
    fwrite(sector + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, payload, f);
    fixture_write_type_count(f, 0, 0);
    edc_write_bytes(edc_compute(0, covered, covered_len), edc);
    fwrite(edc, 1, EDC_SIZE, f);
    rewind(f);
}

/*
 * Test: a type 2 record expands to the 2336-byte body, as the format specifies.
 * Before the fix the decoder emitted 2352 bytes with an invented header, so a
 * spec-conformant stream (literal header + record) grew by 16 bytes per sector.
 */
void test_mode2_record_expands_to_2336(void) {
    TEST(mode2_record_expands_to_2336);

    eccedc_init();

    static const uint8_t msf[3] = {0x00, 0x02, 0x00};
    uint8_t sector[SECTOR_SIZE_RAW];
    fixture_mode2_sector(sector, SECTOR_TYPE_MODE2_FORM1, msf, 5);

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);
    write_mode2_stream(fin, sector, SECTOR_TYPE_MODE2_FORM1, false);

    decode_stats_t stats = {false, false};
    ASSERT_EQ(0, unecmify(fin, fout, &stats, false, false, false));
    ASSERT_TRUE(stats.saw_mode2);

    fseek(fout, 0, SEEK_END);
    ASSERT_EQ_MSG(SECTOR_SIZE_MODE2, ftell(fout), "type 2 record must expand to 2336 bytes");
    rewind(fout);
    uint8_t decoded[SECTOR_SIZE_MODE2];
    ASSERT_EQ(1, fread(decoded, SECTOR_SIZE_MODE2, 1, fout));
    ASSERT_MEM_EQ(sector + OFFSET_MODE2_SUBHEADER, decoded, SECTOR_SIZE_MODE2);

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Decode a literal-header + record stream and require the exact raw sector back.
 */
static bool roundtrips_raw_sector(const uint8_t *sector, sector_type_t form) {
    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    uint8_t decoded[SECTOR_SIZE_RAW];
    bool ok = false;

    if (fin == nullptr || fout == nullptr) {
        printf("FAIL: tmpfile\n");
        goto done;
    }
    write_mode2_stream(fin, sector, form, true);
    if (unecmify(fin, fout, nullptr, false, false, false) != 0) {
        printf("FAIL: decode returned an error\n");
        goto done;
    }
    fseek(fout, 0, SEEK_END);
    if (ftell(fout) != SECTOR_SIZE_RAW) {
        printf("FAIL: expected %d output bytes, got %ld\n", SECTOR_SIZE_RAW, ftell(fout));
        goto done;
    }
    rewind(fout);
    if (fread(decoded, SECTOR_SIZE_RAW, 1, fout) != 1 ||
        memcmp(sector, decoded, SECTOR_SIZE_RAW) != 0) {
        printf("FAIL: decoded sector differs from the original\n");
        goto done;
    }
    ok = true;

done:
    if (fin)
        fclose(fin);
    if (fout)
        fclose(fout);
    return ok;
}

/*
 * Test: the upstream layout reconstructs a raw Mode 2 Form 1 sector byte for byte,
 * including an address that is not the sequential 00:02:00.
 */
void test_mode2_form1_stream_roundtrips_raw_sector(void) {
    TEST(mode2_form1_stream_roundtrips_raw_sector);

    eccedc_init();

    static const uint8_t msf[3] = {0x00, 0x03, 0x00};
    uint8_t sector[SECTOR_SIZE_RAW];
    fixture_mode2_sector(sector, SECTOR_TYPE_MODE2_FORM1, msf, 9);

    ASSERT_TRUE(roundtrips_raw_sector(sector, SECTOR_TYPE_MODE2_FORM1));
    PASS();
}

/*
 * Test: same for Mode 2 Form 2, which has EDC only and no ECC.
 */
void test_mode2_form2_stream_roundtrips_raw_sector(void) {
    TEST(mode2_form2_stream_roundtrips_raw_sector);

    eccedc_init();

    static const uint8_t msf[3] = {0x12, 0x34, 0x56};
    uint8_t sector[SECTOR_SIZE_RAW];
    fixture_mode2_sector(sector, SECTOR_TYPE_MODE2_FORM2, msf, 13);

    ASSERT_TRUE(roundtrips_raw_sector(sector, SECTOR_TYPE_MODE2_FORM2));
    PASS();
}

/*
 * Test: regenerate_mode2_header() derives the MSF address from the output position the way
 * versions 1.2.0 to 1.3.1 did: sector number = bytes / 2352, plus the 150-frame pregap, BCD.
 */
void test_regenerate_mode2_header_addresses(void) {
    TEST(regenerate_mode2_header_addresses);

    static const struct {
        int64_t outbytes;
        uint8_t msf[3];
    } cases[] = {
        {0, {0x00, 0x02, 0x00}},                            /* first sector */
        {1 * SECTOR_SIZE_RAW, {0x00, 0x02, 0x01}},          /* second sector */
        {149 * SECTOR_SIZE_RAW, {0x00, 0x03, 0x74}},        /* last frame of a second */
        {4500 * SECTOR_SIZE_RAW, {0x01, 0x02, 0x00}},       /* minute rollover */
        {4500 * SECTOR_SIZE_RAW + 100, {0x01, 0x02, 0x00}}, /* partial sector counts as its start */
    };
    uint8_t expected_sync[SECTOR_SYNC_HEADER_SIZE];

    memset(expected_sync, 0, sizeof(expected_sync));
    fixture_sync(expected_sync);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t sector[SECTOR_SIZE_RAW];
        memset(sector, 0xAA, sizeof(sector));
        regenerate_mode2_header(sector, cases[i].outbytes);
        ASSERT_MEM_EQ(expected_sync, sector, OFFSET_HEADER);
        ASSERT_MEM_EQ(cases[i].msf, sector + OFFSET_HEADER, MODE1_ADDRESS_SIZE);
        ASSERT_EQ(0x02, sector[OFFSET_MODE]);
        ASSERT_EQ_MSG(0xAA, sector[SECTOR_SYNC_HEADER_SIZE], "body must be left untouched");
    }
    PASS();
}

/*
 * Test: --mode2-2352 restores an archive written by versions 1.2.0 to 1.3.1. Those encoders
 * dropped the 16-byte header of raw Mode 2 sectors, and their decoder regenerated it with a
 * sequential address that counted every output byte, literal runs included. A stream of one
 * literal sector followed by header-less Form 1 and Form 2 records must therefore decode to
 * three consecutive raw sectors at 00:02:00, 00:02:01 and 00:02:02.
 */
void test_mode2_2352_restores_legacy_archive(void) {
    TEST(mode2_2352_restores_legacy_archive);

    eccedc_init();

    static const uint8_t msf0[3] = {0x00, 0x02, 0x00};
    static const uint8_t msf1[3] = {0x00, 0x02, 0x01};
    static const uint8_t msf2[3] = {0x00, 0x02, 0x02};
    uint8_t image[3 * SECTOR_SIZE_RAW];
    uint8_t *s0 = image;
    uint8_t *s1 = image + SECTOR_SIZE_RAW;
    uint8_t *s2 = image + 2 * SECTOR_SIZE_RAW;
    uint8_t edc[EDC_SIZE];
    uint32_t covered = 0;

    fixture_mode2_sector(s0, SECTOR_TYPE_MODE2_FORM1, msf0, 5);
    fixture_mode2_sector(s1, SECTOR_TYPE_MODE2_FORM1, msf1, 7);
    fixture_mode2_sector(s2, SECTOR_TYPE_MODE2_FORM2, msf2, 11);

    FILE *fin = test_tmpfile();
    FILE *fout = test_tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);

    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);
    /* Sector 0 stored literally, in full */
    fixture_write_type_count(fin, SECTOR_TYPE_LITERAL, SECTOR_SIZE_RAW);
    fwrite(s0, 1, SECTOR_SIZE_RAW, fin);
    covered = edc_compute(covered, s0, SECTOR_SIZE_RAW);
    /* Sectors 1 and 2 as header-less records; the legacy EDC covered their 2336-byte bodies */
    fixture_write_type_count(fin, SECTOR_TYPE_MODE2_FORM1, 1);
    fwrite(s1 + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM1_DATA_SIZE, fin);
    covered = edc_compute(covered, s1 + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
    fixture_write_type_count(fin, SECTOR_TYPE_MODE2_FORM2, 1);
    fwrite(s2 + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM2_DATA_SIZE, fin);
    covered = edc_compute(covered, s2 + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
    fixture_write_type_count(fin, 0, 0);
    edc_write_bytes(covered, edc);
    fwrite(edc, 1, EDC_SIZE, fin);
    rewind(fin);

    decode_stats_t stats = {false, false};
    ASSERT_EQ(0, unecmify(fin, fout, &stats, false, false, true));
    ASSERT_TRUE(stats.saw_mode2);

    fseek(fout, 0, SEEK_END);
    ASSERT_EQ_MSG(3 * SECTOR_SIZE_RAW, ftell(fout), "every sector must expand to 2352 bytes");
    rewind(fout);
    uint8_t decoded[3 * SECTOR_SIZE_RAW];
    ASSERT_EQ(1, fread(decoded, sizeof(decoded), 1, fout));
    ASSERT_MEM_EQ(image, decoded, sizeof(image));

    /* Without the option the same stream yields the header-less 2336-byte bodies */
    rewind(fin);
    FILE *fplain = test_tmpfile();
    ASSERT_TRUE(fplain != nullptr);
    ASSERT_EQ(0, unecmify(fin, fplain, nullptr, false, false, false));
    fseek(fplain, 0, SEEK_END);
    ASSERT_EQ(SECTOR_SIZE_RAW + 2 * SECTOR_SIZE_MODE2, ftell(fplain));

    fclose(fplain);
    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: output_finish() reports a flush failure that buffered writes hid.
 * Closing the descriptor underneath the stream makes the final flush fail with EBADF, the
 * same shape as a full disk or a closed pipe at exit.
 */
void test_output_finish_reports_flush_failure(void) {
    TEST(output_finish_reports_flush_failure);

#if defined(_WIN32) || defined(_WIN64)
    /* Closing the descriptor under a live stream makes the Windows CRT fail fast, so write
     * into a pipe whose read end is gone: the deferred write then fails with EPIPE */
    int fds[2];
    ASSERT_EQ(0, _pipe(fds, 4096, _O_BINARY));
    ASSERT_EQ(0, _close(fds[0]));
    FILE *f = _fdopen(fds[1], "wb");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ('x', fputc('x', f)); /* buffered, not yet written */
#else
    /* Close the descriptor under the stream so the deferred write fails with EBADF */
    FILE *f = test_tmpfile();
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ('x', fputc('x', f)); /* buffered, not yet written */
    ASSERT_EQ(0, close(fileno(f)));
#endif

    ASSERT_EQ(-1, output_finish(f, "tmpfile"));
    PASS();
}

/*
 * Test: output_finish() succeeds on a healthy stream.
 */
void test_output_finish_succeeds(void) {
    TEST(output_finish_succeeds);

    FILE *f = test_tmpfile();
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ('x', fputc('x', f));

    ASSERT_EQ(0, output_finish(f, "tmpfile"));
    PASS();
}

/*
 * Main test runner
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    TEST_SUITE_BEGIN("UNECM Unit Tests");

    TEST_CATEGORY("Constants Tests");
    test_constants();
    test_magic_constants();
    test_type_count_max_bits();

    TEST_CATEGORY("\nEDC Computation Tests");
    test_edc_compute();
    test_edc_compute_block();

    TEST_CATEGORY("\nECC/EDC Generation Tests");
    test_eccedc_generate_mode1();
    test_eccedc_generate_mode2_form1();
    test_eccedc_generate_mode2_form2();
    test_write_cue_file_mode1();
    test_write_cue_file_mode2();

    TEST_CATEGORY("\nConsistency Tests");
    test_ecc_consistency();

    TEST_CATEGORY("\nDecoder Error Handling Tests");
    test_unecmify_bad_magic();
    test_unecmify_truncated_header();
    test_unecmify_bad_checksum();
    test_unecmify_empty_data();
    test_unecmify_truncated_type_count();

    TEST_CATEGORY("\nMode 2 Record Tests");
    test_mode2_record_expands_to_2336();
    test_mode2_form1_stream_roundtrips_raw_sector();
    test_mode2_form2_stream_roundtrips_raw_sector();
    test_regenerate_mode2_header_addresses();
    test_mode2_2352_restores_legacy_archive();

    TEST_CATEGORY("\nOutput Safety Tests");
    test_output_finish_reports_flush_failure();
    test_output_finish_succeeds();

    TEST_SUITE_END();
}
