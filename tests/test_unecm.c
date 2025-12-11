#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

/*
 * Unit tests for unecm.c
 * Uses include-based testing to access static functions without modifying source
 */

#include "eccedc.h"

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
 * Test: sector_to_msf produces BCD-encoded MSF addresses
 */
void test_sector_to_msf_bcd(void) {
    TEST(sector_to_msf_bcd);

    uint8_t msf[3];

    /* First data sector (150 frame offset -> 00:02:00) */
    sector_to_msf(0, msf);
    ASSERT_EQ(0x00, msf[0]); /* minutes */
    ASSERT_EQ(0x02, msf[1]); /* seconds */
    ASSERT_EQ(0x00, msf[2]); /* frames */

    /* Check multi-digit BCD conversion (sector -> 01:23:45) */
    sector_to_msf(6120, msf); /* ((1*60+23)*75 + 45) - 150 */
    ASSERT_EQ(0x01, msf[0]);
    ASSERT_EQ(0x23, msf[1]);
    ASSERT_EQ(0x45, msf[2]);

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
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write wrong magic header */
    fputc('X', fin);
    fputc('C', fin);
    fputc('M', fin);
    fputc(0x00, fin);

    rewind(fin);

    /* Should fail with wrong magic */
    int result = unecmify(fin, fout, nullptr, false);
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
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Write only 2 bytes of magic */
    fputc('E', fin);
    fputc('C', fin);

    rewind(fin);

    /* Should fail with truncated header */
    int result = unecmify(fin, fout, nullptr, false);
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
    int result = unecmify(fin, fout, nullptr, false);
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

    int result = unecmify(fin, fout, nullptr, false);
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

    int result = unecmify(fin, fout, nullptr, false);
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
 * Test: sector_to_msf() should output BCD-encoded values, not binary.
 * CD sector addresses use BCD (Binary Coded Decimal).
 * For example, frame 10 should be 0x10 (BCD), not 0x0A (binary).
 */
void test_sector_to_msf_bcd_encoding(void) {
    TEST(sector_to_msf_bcd_encoding);

    uint8_t msf[3];

    /* Sector 0 = MSF 00:02:00 (150 frames offset = 2 seconds) */
    sector_to_msf(0, msf);
    ASSERT_EQ_MSG(0x00, msf[0], "Sector 0: minutes should be BCD 0x00");
    ASSERT_EQ_MSG(0x02, msf[1], "Sector 0: seconds should be BCD 0x02");
    ASSERT_EQ_MSG(0x00, msf[2], "Sector 0: frames should be BCD 0x00");

    /* Sector 10 = 160 frames = 00:02:10 */
    /* Frame 10 should be BCD 0x10, not binary 0x0A */
    sector_to_msf(10, msf);
    ASSERT_EQ_MSG(0x00, msf[0], "Sector 10: minutes should be BCD 0x00");
    ASSERT_EQ_MSG(0x02, msf[1], "Sector 10: seconds should be BCD 0x02");
    ASSERT_EQ_MSG(0x10, msf[2], "Sector 10: frames should be BCD 0x10, not binary 0x0A");

    /* Sector 75 = 225 frames = 00:03:00 (75 frames = 1 second) */
    sector_to_msf(75, msf);
    ASSERT_EQ_MSG(0x00, msf[0], "Sector 75: minutes should be BCD 0x00");
    ASSERT_EQ_MSG(0x03, msf[1], "Sector 75: seconds should be BCD 0x03");
    ASSERT_EQ_MSG(0x00, msf[2], "Sector 75: frames should be BCD 0x00");

    /* Sector 4350 = 4500 frames = 01:00:00 (4500 = 60*75) */
    sector_to_msf(4350, msf);
    ASSERT_EQ_MSG(0x01, msf[0], "Sector 4350: minutes should be BCD 0x01");
    ASSERT_EQ_MSG(0x00, msf[1], "Sector 4350: seconds should be BCD 0x00");
    ASSERT_EQ_MSG(0x00, msf[2], "Sector 4350: frames should be BCD 0x00");

    /* Sector 4500 = 4650 frames = 01:02:00 */
    sector_to_msf(4500, msf);
    ASSERT_EQ_MSG(0x01, msf[0], "Sector 4500: minutes should be BCD 0x01");
    ASSERT_EQ_MSG(0x02, msf[1], "Sector 4500: seconds should be BCD 0x02");
    ASSERT_EQ_MSG(0x00, msf[2], "Sector 4500: frames should be BCD 0x00");

    /* Test a frame value that would differ between binary and BCD: frame=59 */
    /* Sector 59 = 209 frames = 00:02:59 */
    /* Frame 59 should be BCD 0x59, not binary 0x3B */
    sector_to_msf(59, msf);
    ASSERT_EQ_MSG(0x00, msf[0], "Sector 59: minutes should be BCD 0x00");
    ASSERT_EQ_MSG(0x02, msf[1], "Sector 59: seconds should be BCD 0x02");
    ASSERT_EQ_MSG(0x59, msf[2], "Sector 59: frames should be BCD 0x59, not binary 0x3B");

    PASS();
}

/*
 * Test: Mode 2 decoder should output full 2352-byte raw sectors (with sync/header).
 */
void test_mode2_output_size_compatibility(void) {
    TEST(mode2_output_size_compatibility);

    eccedc_init();

    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != nullptr);
    ASSERT_TRUE(fout != nullptr);

    /* Build Mode 2 Form 1 data (2052 bytes that gets stored in ECM) */
    uint8_t mode2_input[MODE2_FORM1_DATA_SIZE];
    memset(mode2_input, 0, sizeof(mode2_input));
    /* Subheader copy (4 bytes): file 0, channel 0, submode 0x08 (Form 1), coding 0 */
    mode2_input[0] = 0x00;
    mode2_input[1] = 0x00;
    mode2_input[2] = 0x08;
    mode2_input[3] = 0x00;
    /* User data: 2048 bytes of zeros (already zeroed) */

    /*
     * Build the full 2336-byte Mode 2 sector to compute the checksum.
     * The decoder builds this internally and computes EDC over it.
     */
    uint8_t full_mode2[SECTOR_SIZE_MODE2];
    memset(full_mode2, 0, sizeof(full_mode2));
    /* Subheader (4 bytes) */
    full_mode2[0] = 0x00;
    full_mode2[1] = 0x00;
    full_mode2[2] = 0x08;
    full_mode2[3] = 0x00;
    /* Subheader copy (4 bytes) */
    full_mode2[4] = 0x00;
    full_mode2[5] = 0x00;
    full_mode2[6] = 0x08;
    full_mode2[7] = 0x00;
    /* User data (2048 bytes at offset 8) - zeros */
    /* EDC/ECC area is also zero initially, but decoder generates it */

    /* For EDC checksum in ECM file: compute over the 2336-byte Mode 2 sector
     * after ECC/EDC generation. We need to call eccedc_generate to get the right values. */
    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));
    /* Sync */
    sector[0] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;
    /* MSF (doesn't matter for EDC) */
    sector[OFFSET_MODE] = 0x02;
    /* Subheader + copy */
    sector[0x10] = sector[0x14] = 0x00;
    sector[0x11] = sector[0x15] = 0x00;
    sector[0x12] = sector[0x16] = 0x08;
    sector[0x13] = sector[0x17] = 0x00;
    /* Generate ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);

    /* Compute the checksum over 2336 bytes starting at offset 0x10 */
    uint32_t expected_edc = edc_compute(0, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);

    /* Write valid ECM magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Write type 2 (Mode 2 Form 1), count 1 */
    fputc(0x02, fin);

    /* Write Mode 2 Form 1 data (2052 bytes) */
    fwrite(mode2_input, 1, MODE2_FORM1_DATA_SIZE, fin);

    /* Write end-of-records marker */
    fputc(0xFC, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0x7F, fin);

    /* Write EDC checksum */
    fputc((expected_edc >> 0) & 0xFF, fin);
    fputc((expected_edc >> 8) & 0xFF, fin);
    fputc((expected_edc >> 16) & 0xFF, fin);
    fputc((expected_edc >> 24) & 0xFF, fin);

    rewind(fin);

    decode_stats_t stats = {false, false};
    int result = unecmify(fin, fout, &stats, false);
    ASSERT_EQ(0, result);

    /* Check output size */
    fseek(fout, 0, SEEK_END);
    long outsize = ftell(fout);

    ASSERT_EQ_MSG(SECTOR_SIZE_RAW, outsize, "Mode 2 output should be full 2352 bytes");

    fclose(fin);
    fclose(fout);
    PASS();
}

/*
 * Test: Mode 2 Form 1 sector is reconstructed to original raw 2352 bytes.
 */
void test_mode2_form1_roundtrip_full_sector(void) {
    TEST(mode2_form1_roundtrip_full_sector);

    eccedc_init();

    /* Build a raw Mode 2 Form 1 sector */
    uint8_t sector[SECTOR_SIZE_RAW];
    memset(sector, 0, sizeof(sector));

    sector[0x00] = SYNC_BYTE_START;
    for (int i = 1; i <= 10; i++)
        sector[i] = SYNC_BYTE_MIDDLE;
    sector[0x0B] = SYNC_BYTE_END;
    sector[OFFSET_MODE] = 0x02;
    sector_to_msf(0, sector + OFFSET_HEADER);

    /* Subheader and duplicate */
    sector[0x10] = 0x00;
    sector[0x11] = 0x00;
    sector[0x12] = 0x08;
    sector[0x13] = 0x00;
    sector[0x14] = sector[0x10];
    sector[0x15] = sector[0x11];
    sector[0x16] = sector[0x12];
    sector[0x17] = sector[0x13];

    /* User data */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[0x18 + i] = (uint8_t)((i * 9) & 0xFF);
    }

    /* Generate ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);

    /* Compute ECM trailing EDC over 2336-byte Mode 2 portion */
    uint32_t ecm_edc = edc_compute(0, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);

    /* Build ECM file in-memory */
    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    ASSERT_TRUE(fin != nullptr && fout != nullptr);

    /* Magic */
    fputc(ECM_MAGIC_E, fin);
    fputc(ECM_MAGIC_C, fin);
    fputc(ECM_MAGIC_M, fin);
    fputc(ECM_MAGIC_NULL, fin);

    /* Type 2, count 1 */
    fputc(0x02, fin);

    /* Payload: subheader copy + user data (matches encoder layout) */
    fwrite(sector + 0x14, 1, MODE2_FORM1_DATA_SIZE, fin);

    /* End marker: type 0, count 0 encoded as 0xFC FF FF FF 7F */
    fputc(0xFC, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0xFF, fin);
    fputc(0x7F, fin);

    /* Trailing EDC (little-endian) */
    fputc((ecm_edc >> 0) & 0xFF, fin);
    fputc((ecm_edc >> 8) & 0xFF, fin);
    fputc((ecm_edc >> 16) & 0xFF, fin);
    fputc((ecm_edc >> 24) & 0xFF, fin);

    rewind(fin);

    /* Decode */
    int result = unecmify(fin, fout, nullptr, false);
    ASSERT_EQ(0, result);

    /* Verify output matches original sector */
    rewind(fout);
    uint8_t decoded[SECTOR_SIZE_RAW];
    ASSERT_EQ(1, fread(decoded, SECTOR_SIZE_RAW, 1, fout));
    ASSERT_MEM_EQ(sector, decoded, SECTOR_SIZE_RAW);

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

    TEST_SUITE_BEGIN("UNECM Unit Tests");

    TEST_CATEGORY("Constants Tests");
    test_constants();
    test_magic_constants();
    test_type_count_max_bits();
    test_sector_to_msf_bcd();

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

    TEST_CATEGORY("\nMSF Address Tests");
    test_sector_to_msf_bcd_encoding();

    TEST_CATEGORY("\nMode 2 Compatibility Tests");
    test_mode2_output_size_compatibility();
    test_mode2_form1_roundtrip_full_sector();

    TEST_SUITE_END();
}
