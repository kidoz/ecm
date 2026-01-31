/*
 * Performance benchmarks for ECM
 *
 * Measures throughput of key operations:
 * - EDC computation
 * - ECC generation
 * - Sector type detection
 * - Full encode/decode
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "eccedc.h"

/* Include ecm.c static functions */
#define main   ecm_main
#define banner ecm_banner
#include "../src/ecm.c"
#undef main
#undef banner

/* Include unecm.c static functions */
#define main            unecm_main
#define banner          unecm_banner
#define progress_reset  unecm_progress_reset
#define progress_update unecm_progress_update
#define progress_t      unecm_progress_t
#define is_stdio        unecm_is_stdio
#include "../src/unecm.c"
#undef main
#undef banner
#undef progress_reset
#undef progress_update
#undef progress_t
#undef is_stdio

/*
 * Timing utilities using CLOCK_MONOTONIC
 */
typedef struct {
    struct timespec start;
    struct timespec end;
} bench_timer_t;

static void timer_start(bench_timer_t *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->start);
}

static void timer_stop(bench_timer_t *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->end);
}

static double timer_elapsed_ms(const bench_timer_t *t) {
    double start_ms = t->start.tv_sec * 1000.0 + t->start.tv_nsec / 1000000.0;
    double end_ms = t->end.tv_sec * 1000.0 + t->end.tv_nsec / 1000000.0;
    return end_ms - start_ms;
}

static double timer_elapsed_us(const bench_timer_t *t) {
    double start_us = t->start.tv_sec * 1000000.0 + t->start.tv_nsec / 1000.0;
    double end_us = t->end.tv_sec * 1000000.0 + t->end.tv_nsec / 1000.0;
    return end_us - start_us;
}

/*
 * Test data: Valid Mode 1 sector
 */
static void create_mode1_sector(uint8_t *sector) {
    memset(sector, 0, SECTOR_SIZE_RAW);

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

    /* User data with pattern */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[OFFSET_MODE1_DATA + i] = (uint8_t)(i & 0xFF);
    }

    /* Generate valid ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE1);
}

/*
 * Test data: Valid Mode 2 Form 1 sector
 */
static void create_mode2_form1_sector(uint8_t *sector) {
    memset(sector, 0, SECTOR_SIZE_RAW);

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

    /* User data with pattern */
    for (int i = 0; i < SECTOR_USER_DATA; i++) {
        sector[0x18 + i] = (uint8_t)((i * 7) & 0xFF);
    }

    /* Generate ECC/EDC */
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);
}

/*
 * Benchmark: EDC computation
 */
static void benchmark_edc_compute(void) {
    const size_t sizes[] = {1024, 2048, SECTOR_SIZE_RAW, 1024 * 1024};
    const char *size_names[] = {"1 KB", "2 KB", "2352 B (sector)", "1 MB"};
    const int iterations = 10000;

    printf("EDC Computation:\n");

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint8_t *data = malloc(sizes[i]);
        if (!data) {
            printf("  Error: failed to allocate %zu bytes\n", sizes[i]);
            continue;
        }

        /* Fill with pattern */
        for (size_t j = 0; j < sizes[i]; j++) {
            data[j] = (uint8_t)(j & 0xFF);
        }

        /* Adjust iterations for larger sizes */
        int iters = (sizes[i] >= 1024 * 1024) ? 100 : iterations;

        bench_timer_t timer;
        timer_start(&timer);
        for (int iter = 0; iter < iters; iter++) {
            (void)edc_compute(0, data, sizes[i]);
        }
        timer_stop(&timer);

        double elapsed_ms = timer_elapsed_ms(&timer);
        double total_bytes = (double)sizes[i] * iters;
        double throughput_mbs = (total_bytes / (1024 * 1024)) / (elapsed_ms / 1000.0);

        printf("  %s: %.1f MB/s (%d iterations, %.2f ms)\n", size_names[i], throughput_mbs, iters,
               elapsed_ms);

        free(data);
    }
}

/*
 * Benchmark: ECC generation
 */
static void benchmark_ecc_generate(void) {
    const int iterations = 10000;
    uint8_t sector[SECTOR_SIZE_RAW];

    printf("\nECC Generation:\n");

    /* Mode 1 sector */
    create_mode1_sector(sector);

    bench_timer_t timer;
    timer_start(&timer);
    for (int i = 0; i < iterations; i++) {
        ecc_generate(sector, false);
    }
    timer_stop(&timer);

    double elapsed_us = timer_elapsed_us(&timer);
    double us_per_sector = elapsed_us / iterations;
    double sectors_per_sec = 1000000.0 / us_per_sector;

    printf("  Mode 1 (no zero addr): %.2f us/sector (%.0f sectors/sec)\n", us_per_sector,
           sectors_per_sec);

    /* Mode 2 Form 1 sector (with address zeroing) */
    create_mode2_form1_sector(sector);

    timer_start(&timer);
    for (int i = 0; i < iterations; i++) {
        ecc_generate(sector, true);
    }
    timer_stop(&timer);

    elapsed_us = timer_elapsed_us(&timer);
    us_per_sector = elapsed_us / iterations;
    sectors_per_sec = 1000000.0 / us_per_sector;

    printf("  Mode 2 Form 1 (zero addr): %.2f us/sector (%.0f sectors/sec)\n", us_per_sector,
           sectors_per_sec);
}

/*
 * Benchmark: Sector type detection
 */
static void benchmark_check_type(void) {
    const int iterations = 100000;
    uint8_t sector[SECTOR_SIZE_RAW];

    printf("\nSector Type Detection:\n");

    /* Mode 1 sector */
    create_mode1_sector(sector);

    bench_timer_t timer;
    timer_start(&timer);
    for (int i = 0; i < iterations; i++) {
        (void)check_type_raw(sector);
    }
    timer_stop(&timer);

    double elapsed_us = timer_elapsed_us(&timer);
    double us_per_sector = elapsed_us / iterations;
    double sectors_per_sec = 1000000.0 / us_per_sector;

    printf("  Mode 1: %.2f us/sector (%.0f sectors/sec)\n", us_per_sector, sectors_per_sec);

    /* Mode 2 Form 1 sector */
    create_mode2_form1_sector(sector);

    timer_start(&timer);
    for (int i = 0; i < iterations; i++) {
        (void)check_type_raw(sector);
    }
    timer_stop(&timer);

    elapsed_us = timer_elapsed_us(&timer);
    us_per_sector = elapsed_us / iterations;
    sectors_per_sec = 1000000.0 / us_per_sector;

    printf("  Mode 2 Form 1: %.2f us/sector (%.0f sectors/sec)\n", us_per_sector, sectors_per_sec);

    /* Literal (random data) */
    for (int i = 0; i < SECTOR_SIZE_RAW; i++) {
        sector[i] = (uint8_t)(i * 13 + 7);
    }

    timer_start(&timer);
    for (int i = 0; i < iterations; i++) {
        (void)check_type_raw(sector);
    }
    timer_stop(&timer);

    elapsed_us = timer_elapsed_us(&timer);
    us_per_sector = elapsed_us / iterations;
    sectors_per_sec = 1000000.0 / us_per_sector;

    printf("  Literal (early reject): %.2f us/sector (%.0f sectors/sec)\n", us_per_sector,
           sectors_per_sec);
}

/*
 * Benchmark: Full encode throughput
 */
static void benchmark_encode(void) {
    const int num_sectors = 1000;
    uint8_t sector[SECTOR_SIZE_RAW];

    printf("\nFull Encode Throughput:\n");

    /* Create test data: many Mode 1 sectors */
    create_mode1_sector(sector);

    FILE *fin = tmpfile();
    FILE *fout = tmpfile();
    if (!fin || !fout) {
        printf("  Error: failed to create temp files\n");
        if (fin)
            fclose(fin);
        if (fout)
            fclose(fout);
        return;
    }

    /* Write sectors */
    for (int i = 0; i < num_sectors; i++) {
        /* Update MSF address */
        uint32_t frame = (uint32_t)i + 150;
        sector[OFFSET_HEADER + 2] = (uint8_t)(frame % 75);
        frame /= 75;
        sector[OFFSET_HEADER + 1] = (uint8_t)(frame % 60);
        sector[OFFSET_HEADER + 0] = (uint8_t)(frame / 60);

        /* Regenerate ECC/EDC */
        eccedc_generate(sector, SECTOR_TYPE_MODE1);

        fwrite(sector, SECTOR_SIZE_RAW, 1, fin);
    }
    rewind(fin);

    size_t input_size = (size_t)num_sectors * SECTOR_SIZE_RAW;

    bench_timer_t timer;
    timer_start(&timer);
    int result = ecmify(fin, fout, false);
    timer_stop(&timer);

    if (result != 0) {
        printf("  Error: encoding failed\n");
    } else {
        double elapsed_ms = timer_elapsed_ms(&timer);
        double throughput_mbs = (input_size / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
        printf("  Batch mode (%d sectors): %.1f MB/s (%.2f ms)\n", num_sectors, throughput_mbs,
               elapsed_ms);
    }

    /* Test streaming mode */
    rewind(fin);
    FILE *fout2 = tmpfile();
    if (!fout2) {
        printf("  Error: failed to create temp file\n");
        fclose(fin);
        fclose(fout);
        return;
    }

    timer_start(&timer);
    result = ecmify_streaming(fin, fout2, false);
    timer_stop(&timer);

    if (result != 0) {
        printf("  Error: streaming encoding failed\n");
    } else {
        double elapsed_ms = timer_elapsed_ms(&timer);
        double throughput_mbs = (input_size / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
        printf("  Streaming mode (%d sectors): %.1f MB/s (%.2f ms)\n", num_sectors, throughput_mbs,
               elapsed_ms);
    }

    fclose(fin);
    fclose(fout);
    fclose(fout2);
}

/*
 * Benchmark: Full decode throughput
 */
static void benchmark_decode(void) {
    const int num_sectors = 1000;
    uint8_t sector[SECTOR_SIZE_RAW];

    printf("\nFull Decode Throughput:\n");

    /* Create encoded data first */
    create_mode1_sector(sector);

    FILE *fin = tmpfile();
    FILE *fenc = tmpfile();
    if (!fin || !fenc) {
        printf("  Error: failed to create temp files\n");
        if (fin)
            fclose(fin);
        if (fenc)
            fclose(fenc);
        return;
    }

    /* Write sectors */
    for (int i = 0; i < num_sectors; i++) {
        uint32_t frame = (uint32_t)i + 150;
        sector[OFFSET_HEADER + 2] = (uint8_t)(frame % 75);
        frame /= 75;
        sector[OFFSET_HEADER + 1] = (uint8_t)(frame % 60);
        sector[OFFSET_HEADER + 0] = (uint8_t)(frame / 60);
        eccedc_generate(sector, SECTOR_TYPE_MODE1);
        fwrite(sector, SECTOR_SIZE_RAW, 1, fin);
    }
    rewind(fin);

    /* Encode */
    int result = ecmify(fin, fenc, false);
    if (result != 0) {
        printf("  Error: encoding failed\n");
        fclose(fin);
        fclose(fenc);
        return;
    }
    rewind(fenc);

    /* Get encoded size */
    fseek(fenc, 0, SEEK_END);
    long encoded_size = ftell(fenc);
    rewind(fenc);

    size_t output_size = (size_t)num_sectors * SECTOR_SIZE_RAW;

    /* Decode */
    FILE *fout = tmpfile();
    if (!fout) {
        printf("  Error: failed to create temp file\n");
        fclose(fin);
        fclose(fenc);
        return;
    }

    bench_timer_t timer;
    timer_start(&timer);
    result = unecmify(fenc, fout, nullptr, false, false);
    timer_stop(&timer);

    if (result != 0) {
        printf("  Error: decoding failed\n");
    } else {
        double elapsed_ms = timer_elapsed_ms(&timer);
        double throughput_mbs = (output_size / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
        printf("  Decode (%d sectors, %ld encoded bytes): %.1f MB/s (%.2f ms)\n", num_sectors,
               encoded_size, throughput_mbs, elapsed_ms);
    }

    fclose(fin);
    fclose(fenc);
    fclose(fout);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== ECM Performance Benchmarks ===\n\n");

    eccedc_init();

    printf("Micro-benchmarks:\n");
    printf("-----------------\n");
    benchmark_edc_compute();
    benchmark_ecc_generate();
    benchmark_check_type();

    printf("\n");
    printf("End-to-end:\n");
    printf("-----------\n");
    benchmark_encode();
    benchmark_decode();

    printf("\n=== Benchmark Complete ===\n");

    return 0;
}
