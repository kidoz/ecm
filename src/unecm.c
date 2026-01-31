#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(_WIN32) || defined(_WIN64)
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

#include "eccedc.h"
#include "version.h"

static void banner(void) {
    fprintf(stderr, "UNECM - Decoder for Error Code Modeler format v" ECM_VERSION "\n\n");
}

/*
 * Progress tracking
 */
typedef struct {
    int64_t current;
    int64_t total;
} progress_t;

static void progress_reset(progress_t *p, int64_t total) {
    p->current = 0;
    p->total = total;
}

static void progress_update(progress_t *p, int64_t n) {
    if ((n >> 20) != (p->current >> 20)) {
        int64_t d = (p->total + 64) / 128;
        if (!d)
            d = 1;
        fprintf(stderr, "Decoding (%02u%%)\r", (unsigned)((100 * ((n + 64) / 128)) / d));
    }
    p->current = n;
}

/*
 * Decode statistics for CUE file generation
 */
typedef struct {
    bool saw_mode1;
    bool saw_mode2;
} decode_stats_t;

/*
 * Output position tracker for MSF address computation.
 * Tracks bytes written explicitly to support non-seekable outputs (stdout/pipes).
 * Literal bytes also contribute to the sector position.
 */
typedef struct {
    int64_t bytes_written;
} output_tracker_t;

/* Convert binary value to packed BCD (00-99) */
static uint8_t to_bcd(uint8_t value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/*
 * Convert sector number to MSF (Minutes:Seconds:Frames) address.
 * CD standard: 75 frames/second, 60 seconds/minute.
 * First data sector is at MSF 00:02:00 (150 frames offset).
 */
static void sector_to_msf(uint32_t sector, uint8_t *msf) {
    uint32_t frame = sector + 150; /* 2-second pregap offset */
    msf[2] = to_bcd((uint8_t)(frame % 75));
    frame /= 75;
    msf[1] = to_bcd((uint8_t)(frame % 60));
    msf[0] = to_bcd((uint8_t)(frame / 60));
}

/*
 * Get current sector number from tracked output position.
 */
static uint32_t get_current_sector(const output_tracker_t *tracker) {
    return (uint32_t)(tracker->bytes_written / SECTOR_SIZE_RAW);
}

/*
 * Initialize a Mode 2 sector with sync pattern, MSF address, and mode byte.
 */
static void init_mode2_sector(uint8_t *sector, uint32_t sector_num) {
    memset(sector, 0, SECTOR_SIZE_RAW);
    sector_init_sync(sector);
    sector_to_msf(sector_num, sector + OFFSET_HEADER);
    sector[OFFSET_MODE] = 0x02;
}

/*
 * Read and verify magic header
 */
static bool read_magic_header(FILE *in) {
    int magic[4];
    if ((magic[0] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }
    if ((magic[1] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }
    if ((magic[2] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }
    if ((magic[3] = fgetc(in)) == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        return false;
    }

    if (magic[0] != ECM_MAGIC_E || magic[1] != ECM_MAGIC_C || magic[2] != ECM_MAGIC_M ||
        magic[3] != ECM_MAGIC_NULL) {
        fprintf(stderr, "Header not found!\n");
        return false;
    }
    return true;
}

/*
 * Read type/count from input
 */
static bool read_type_count(FILE *in, unsigned *type, unsigned *num) {
    int c = fgetc(in);
    int bits = 5;

    if (c == EOF)
        return false;

    *type = (unsigned)(c & 3);
    *num = (unsigned)((c >> 2) & 0x1F);

    while (c & 0x80) {
        c = fgetc(in);
        if (c == EOF)
            return false;
        if (bits >= TYPE_COUNT_MAX_BITS) {
            fprintf(stderr, "Error: invalid type/count encoding (overflow)\n");
            return false;
        }
        *num |= ((unsigned)(c & 0x7F)) << bits;
        bits += 7;
    }
    return true;
}

/*
 * Decode a Mode 1 sector
 */
static int decode_mode1_sector(FILE *in, FILE *out, uint8_t *sector, uint32_t *checkedc,
                               output_tracker_t *tracker) {
    memset(sector, 0, SECTOR_SIZE_RAW);
    sector_init_sync(sector);
    sector[OFFSET_MODE] = 0x01;

    if (fread(sector + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, in) != MODE1_ADDRESS_SIZE) {
        return -1;
    }
    if (fread(sector + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, in) != SECTOR_USER_DATA) {
        return -1;
    }
    eccedc_generate(sector, SECTOR_TYPE_MODE1);
    *checkedc = edc_compute(*checkedc, sector, SECTOR_SIZE_RAW);
    if (fwrite(sector, SECTOR_SIZE_RAW, 1, out) != 1) {
        fprintf(stderr, "Error: failed to write output\n");
        return -2;
    }
    tracker->bytes_written += SECTOR_SIZE_RAW;
    return 0;
}

/*
 * Decode a Mode 2 Form 1 sector
 */
static int decode_mode2_form1_sector(FILE *in, FILE *out, uint8_t *sector, uint32_t *checkedc,
                                     output_tracker_t *tracker) {
    uint32_t sector_num = get_current_sector(tracker);
    init_mode2_sector(sector, sector_num);

    if (fread(sector + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM1_DATA_SIZE,
              in) != MODE2_FORM1_DATA_SIZE) {
        return -1;
    }
    sector_copy_subheader(sector);
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);
    *checkedc = edc_compute(*checkedc, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
    if (fwrite(sector, SECTOR_SIZE_RAW, 1, out) != 1) {
        fprintf(stderr, "Error: failed to write output\n");
        return -2;
    }
    tracker->bytes_written += SECTOR_SIZE_RAW;
    return 0;
}

/*
 * Decode a Mode 2 Form 2 sector
 */
static int decode_mode2_form2_sector(FILE *in, FILE *out, uint8_t *sector, uint32_t *checkedc,
                                     output_tracker_t *tracker) {
    uint32_t sector_num = get_current_sector(tracker);
    init_mode2_sector(sector, sector_num);

    if (fread(sector + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM2_DATA_SIZE,
              in) != MODE2_FORM2_DATA_SIZE) {
        return -1;
    }
    sector_copy_subheader(sector);
    eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM2);
    *checkedc = edc_compute(*checkedc, sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
    if (fwrite(sector, SECTOR_SIZE_RAW, 1, out) != 1) {
        fprintf(stderr, "Error: failed to write output\n");
        return -2;
    }
    tracker->bytes_written += SECTOR_SIZE_RAW;
    return 0;
}

/*
 * Main decoding function
 */
static int unecmify(FILE *in, FILE *out, decode_stats_t *stats, bool is_stdin, bool verbose) {
    static const char *type_names[] = {"literal", "mode1", "mode2f1", "mode2f2"};
    uint32_t checkedc = 0;
    uint8_t sector[SECTOR_SIZE_RAW];
    progress_t progress;
    output_tracker_t tracker = {0};

    /* For regular files, get size for progress tracking; for stdin, skip */
    if (!is_stdin) {
        if (fseeko(in, 0, SEEK_END) != 0) {
            fprintf(stderr, "Error: failed to seek input file\n");
            return 1;
        }
        off_t endpos = ftello(in);
        if (endpos < 0) {
            fprintf(stderr, "Error: failed to determine input file size\n");
            return 1;
        }
        progress_reset(&progress, endpos);
        if (fseeko(in, 0, SEEK_SET) != 0) {
            fprintf(stderr, "Error: failed to rewind input file\n");
            return 1;
        }
    } else {
        progress_reset(&progress, 0);
    }

    if (!read_magic_header(in)) {
        goto corrupt;
    }

    for (;;) {
        unsigned type, num;
        if (!read_type_count(in, &type, &num)) {
            goto uneof;
        }

        if (num == 0xFFFFFFFF)
            break;
        num++;
        if (num >= 0x80000000)
            goto corrupt;

        ECM_VERBOSE(verbose, "Record: type=%s, count=%u", type_names[type], num);

        if (type == SECTOR_TYPE_LITERAL) {
            while (num) {
                unsigned b = (num > SECTOR_SIZE_RAW) ? SECTOR_SIZE_RAW : num;
                if (fread(sector, 1, b, in) != b) {
                    goto uneof;
                }
                checkedc = edc_compute(checkedc, sector, b);
                if (fwrite(sector, 1, b, out) != b) {
                    fprintf(stderr, "Error: failed to write output\n");
                    goto writeerr;
                }
                tracker.bytes_written += b;
                num -= b;
                off_t pos = ftello(in);
                if (pos >= 0)
                    progress_update(&progress, pos);
            }
            continue;
        }

        while (num--) {
            int ret;
            switch ((sector_type_t)type) {
                case SECTOR_TYPE_MODE1:
                    if (stats)
                        stats->saw_mode1 = true;
                    ret = decode_mode1_sector(in, out, sector, &checkedc, &tracker);
                    break;
                case SECTOR_TYPE_MODE2_FORM1:
                    if (stats)
                        stats->saw_mode2 = true;
                    ret = decode_mode2_form1_sector(in, out, sector, &checkedc, &tracker);
                    break;
                case SECTOR_TYPE_MODE2_FORM2:
                    if (stats)
                        stats->saw_mode2 = true;
                    ret = decode_mode2_form2_sector(in, out, sector, &checkedc, &tracker);
                    break;
                default:
                    fprintf(stderr, "Error: invalid sector type %u\n", type);
                    goto corrupt;
            }
            if (ret == -1)
                goto uneof;
            if (ret == -2)
                goto writeerr;

            off_t pos = ftello(in);
            if (pos >= 0)
                progress_update(&progress, pos);
        }
    }

    /* Read and verify final EDC */
    if (fread(sector, 1, EDC_SIZE, in) != EDC_SIZE) {
        goto uneof;
    }

    off_t inpos = ftello(in);
    off_t outpos = ftello(out);
    fprintf(stderr, "Decoded %lld bytes -> %lld bytes\n", (long long)(inpos >= 0 ? inpos : 0),
            (long long)(outpos >= 0 ? outpos : 0));

    if (sector[0] != ((checkedc >> 0) & 0xFF) || sector[1] != ((checkedc >> 8) & 0xFF) ||
        sector[2] != ((checkedc >> 16) & 0xFF) || sector[3] != ((checkedc >> 24) & 0xFF)) {
        fprintf(stderr, "EDC error (%08X, should be %02X%02X%02X%02X)\n", checkedc, sector[3],
                sector[2], sector[1], sector[0]);
        goto corrupt;
    }

    fprintf(stderr, "Done; file is OK\n");
    return 0;

uneof:
    fprintf(stderr, "Unexpected EOF!\n");
corrupt:
    fprintf(stderr, "Corrupt ECM file!\n");
    return 1;

writeerr:
    return 1;
}

/*
 * Determine track mode string for the CUE sheet
 */
static const char *cue_track_mode(const decode_stats_t *stats) {
    if (stats && !stats->saw_mode2) {
        return "MODE1/2352";
    }
    return "MODE2/2352";
}

/*
 * Write a CUE file for the decoded BIN
 */
static int write_cue_file(const char *outfilename, const decode_stats_t *stats) {
    size_t outlen = strlen(outfilename);
    char *cuefilename = nullptr;
    FILE *cuefile = nullptr;
    int result = 0;

    cuefilename = malloc(outlen + 5);
    if (!cuefilename) {
        fprintf(stderr, "Error: failed to allocate cue filename\n");
        return 1;
    }

    memcpy(cuefilename, outfilename, outlen);
    memcpy(cuefilename + outlen, ".cue", 5);

    cuefile = fopen(cuefilename, "wt");
    if (!cuefile) {
        perror(cuefilename);
        result = 1;
        goto cleanup;
    }

    fprintf(cuefile, "FILE \"%s\" BINARY\n", outfilename);
    fprintf(cuefile, "  TRACK 01 %s\n", cue_track_mode(stats));
    fprintf(cuefile, "    INDEX 01 00:00:00\n");

    fprintf(stderr, "Created CUE file: %s\n", cuefilename);

cleanup:
    if (cuefile) {
        fclose(cuefile);
    }
    free(cuefilename);
    return result;
}

/*
 * Check if filename is "-" for stdin/stdout
 */
static bool is_stdio(const char *filename) {
    return filename[0] == '-' && filename[1] == '\0';
}

int main(int argc, char **argv) {
    FILE *fin = nullptr;
    FILE *fout = nullptr;
    const char *infilename;
    char *outfilename = nullptr;
    bool outfilename_allocated = false;
    bool createcue = false;
    bool verbose = false;
    int result = 0;
    int argoffset = 0;
    decode_stats_t stats = {false, false};

    banner();
    eccedc_init();

    if (argc < 2 || argc > 5) {
        fprintf(stderr, "usage: %s [-v|--verbose] [--cue] ecmfile [outputfile]\n", argv[0]);
        fprintf(stderr, "       use '-' for stdin/stdout\n");
        return 1;
    }

    /* Check for -v/--verbose option */
    if (argc >= 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0)) {
        verbose = true;
        argoffset = 1;
    }

    /* Check for --cue option */
    if (argc >= 2 + argoffset && strcasecmp(argv[1 + argoffset], "--cue") == 0) {
        createcue = true;
        argoffset++;
    }

    if (argc < 2 + argoffset) {
        fprintf(stderr, "usage: %s [-v|--verbose] [--cue] ecmfile [outputfile]\n", argv[0]);
        return 1;
    }

    infilename = argv[1 + argoffset];

    /* Verify input filename ends with .ecm (unless stdin) */
    if (!is_stdio(infilename)) {
        size_t inlen = strlen(infilename);
        if (inlen < 5) {
            fprintf(stderr, "filename '%s' is too short\n", infilename);
            return 1;
        }
        if (strcasecmp(infilename + inlen - 4, ".ecm") != 0) {
            fprintf(stderr, "filename must end in .ecm\n");
            return 1;
        }
    }

    /* Figure out output filename */
    if (argc == 3 + argoffset) {
        outfilename = argv[2 + argoffset];
    } else if (is_stdio(infilename)) {
        outfilename = "-";
    } else {
        size_t inlen = strlen(infilename);
        outfilename = malloc(inlen - 3);
        if (!outfilename) {
            fprintf(stderr, "Error: failed to allocate output filename\n");
            return 1;
        }
        outfilename_allocated = true;
        memcpy(outfilename, infilename, inlen - 4);
        outfilename[inlen - 4] = '\0';
    }

    fprintf(stderr, "Decoding %s to %s.\n", infilename, outfilename);

    /* Open input */
    if (is_stdio(infilename)) {
        fin = stdin;
    } else {
        fin = fopen(infilename, "rb");
        if (!fin) {
            perror(infilename);
            result = 1;
            goto cleanup;
        }
    }

    /* Open output */
    if (is_stdio(outfilename)) {
        fout = stdout;
    } else {
        fout = fopen(outfilename, "wb");
        if (!fout) {
            perror(outfilename);
            result = 1;
            goto cleanup;
        }
    }

    result = unecmify(fin, fout, &stats, is_stdio(infilename), verbose);

    /* Close output file before creating CUE */
    if (fout && !is_stdio(outfilename)) {
        fclose(fout);
        fout = nullptr;
    }

    /* Write CUE file if requested */
    if (result == 0 && createcue && !is_stdio(outfilename)) {
        result = write_cue_file(outfilename, &stats);
    }

cleanup:
    if (fout && !is_stdio(outfilename)) {
        fclose(fout);
    }
    if (fin && !is_stdio(infilename)) {
        fclose(fin);
    }
    if (outfilename_allocated) {
        free(outfilename);
    }

    return result;
}
