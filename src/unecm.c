#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
 * Progress tracking (using int64_t for large file support)
 */
typedef struct {
    int64_t current;
    int64_t total;
} progress_t;

static void progress_reset(progress_t *p, int64_t total) {
    p->current = 0;
    p->total = total;
}

static void progress_set(progress_t *p, int64_t n) {
    if ((n >> 20) != (p->current >> 20)) {
        int64_t a = (n + 64) / 128;
        int64_t d = (p->total + 64) / 128;
        if (!d)
            d = 1;
        fprintf(stderr, "Decoding (%02u%%)\r", (unsigned)((100 * a) / d));
    }
    p->current = n;
}

/*
 * Main decoding function
 */
typedef struct {
    int saw_mode1;
    int saw_mode2;
} decode_stats_t;

static int unecmify(FILE *in, FILE *out, decode_stats_t *stats) {
    uint32_t checkedc = 0;
    uint8_t sector[SECTOR_SIZE_RAW];
    unsigned type;
    unsigned num;
    progress_t progress;

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

    /* Verify magic header */
    int magic[4];
    magic[0] = fgetc(in);
    magic[1] = fgetc(in);
    magic[2] = fgetc(in);
    magic[3] = fgetc(in);
    if (magic[0] == EOF || magic[1] == EOF || magic[2] == EOF || magic[3] == EOF) {
        fprintf(stderr, "Error: failed to read header\n");
        goto uneof;
    }
    if (magic[0] != ECM_MAGIC_E || magic[1] != ECM_MAGIC_C || magic[2] != ECM_MAGIC_M ||
        magic[3] != ECM_MAGIC_NULL) {
        fprintf(stderr, "Header not found!\n");
        goto corrupt;
    }

    for (;;) {
        int c = fgetc(in);
        int bits = 5;

        if (c == EOF) {
            goto uneof;
        }

        type = (unsigned)(c & 3);
        num = (unsigned)((c >> 2) & 0x1F);

        while (c & 0x80) {
            c = fgetc(in);
            if (c == EOF) {
                goto uneof;
            }
            if (bits >= TYPE_COUNT_MAX_BITS) {
                fprintf(stderr, "Error: invalid type/count encoding (overflow)\n");
                goto corrupt;
            }
            num |= ((unsigned)(c & 0x7F)) << bits;
            bits += 7;
        }

        if (num == 0xFFFFFFFF) {
            break;
        }
        num++;

        if (num >= 0x80000000) {
            goto corrupt;
        }

        if (type == SECTOR_TYPE_LITERAL) {
            while (num) {
                unsigned b = num;
                if (b > SECTOR_SIZE_RAW) {
                    b = SECTOR_SIZE_RAW;
                }
                size_t bytes_read = fread(sector, 1, b, in);
                if (bytes_read != b) {
                    goto uneof;
                }
                checkedc = edc_compute(checkedc, sector, b);
                if (fwrite(sector, 1, b, out) != b) {
                    fprintf(stderr, "Error: failed to write output\n");
                    goto writeerr;
                }
                num -= b;
                off_t pos = ftello(in);
                if (pos >= 0) {
                    progress_set(&progress, pos);
                }
            }
        } else {
            while (num--) {
                memset(sector, 0, sizeof(sector));
                /* Set sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00 */
                memset(sector + 1, SYNC_BYTE_MIDDLE, 10);

                switch (type) {
                    case SECTOR_TYPE_MODE1:
                        if (stats) {
                            stats->saw_mode1 = 1;
                        }
                        sector[OFFSET_MODE] = 0x01;
                        /* Read address (3 bytes) */
                        if (fread(sector + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, in) !=
                            MODE1_ADDRESS_SIZE) {
                            goto uneof;
                        }
                        /* Read user data (2048 bytes) */
                        if (fread(sector + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, in) !=
                            SECTOR_USER_DATA) {
                            goto uneof;
                        }
                        /* Generate ECC/EDC */
                        eccedc_generate(sector, SECTOR_TYPE_MODE1);
                        checkedc = edc_compute(checkedc, sector, SECTOR_SIZE_RAW);
                        if (fwrite(sector, SECTOR_SIZE_RAW, 1, out) != 1) {
                            fprintf(stderr, "Error: failed to write output\n");
                            goto writeerr;
                        }
                        {
                            off_t pos = ftello(in);
                            if (pos >= 0) {
                                progress_set(&progress, pos);
                            }
                        }
                        break;

                    case SECTOR_TYPE_MODE2_FORM1:
                        if (stats) {
                            stats->saw_mode2 = 1;
                        }
                        sector[OFFSET_MODE] = 0x02;
                        /* Read subheader + data (4 + 2048 = 2052 bytes) */
                        if (fread(sector + 0x014, 1, MODE2_FORM1_DATA_SIZE, in) !=
                            MODE2_FORM1_DATA_SIZE) {
                            goto uneof;
                        }
                        /* Copy subheader */
                        sector[0x10] = sector[0x14];
                        sector[0x11] = sector[0x15];
                        sector[0x12] = sector[0x16];
                        sector[0x13] = sector[0x17];
                        /* Generate ECC/EDC */
                        eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM1);
                        checkedc = edc_compute(checkedc, sector + OFFSET_MODE2_SUBHEADER,
                                               SECTOR_SIZE_MODE2);
                        if (fwrite(sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2, 1, out) !=
                            1) {
                            fprintf(stderr, "Error: failed to write output\n");
                            goto writeerr;
                        }
                        {
                            off_t pos = ftello(in);
                            if (pos >= 0) {
                                progress_set(&progress, pos);
                            }
                        }
                        break;

                    case SECTOR_TYPE_MODE2_FORM2:
                        if (stats) {
                            stats->saw_mode2 = 1;
                        }
                        sector[OFFSET_MODE] = 0x02;
                        /* Read subheader + data (4 + 2324 = 2328 bytes) */
                        if (fread(sector + 0x014, 1, MODE2_FORM2_DATA_SIZE, in) !=
                            MODE2_FORM2_DATA_SIZE) {
                            goto uneof;
                        }
                        /* Copy subheader */
                        sector[0x10] = sector[0x14];
                        sector[0x11] = sector[0x15];
                        sector[0x12] = sector[0x16];
                        sector[0x13] = sector[0x17];
                        /* Generate EDC (no ECC for Form 2) */
                        eccedc_generate(sector, SECTOR_TYPE_MODE2_FORM2);
                        checkedc = edc_compute(checkedc, sector + OFFSET_MODE2_SUBHEADER,
                                               SECTOR_SIZE_MODE2);
                        if (fwrite(sector + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2, 1, out) !=
                            1) {
                            fprintf(stderr, "Error: failed to write output\n");
                            goto writeerr;
                        }
                        {
                            off_t pos = ftello(in);
                            if (pos >= 0) {
                                progress_set(&progress, pos);
                            }
                        }
                        break;

                    default:
                        /* [TS 17961 5.17 swtchdflt] Invalid sector type */
                        fprintf(stderr, "Error: invalid sector type %u\n", type);
                        goto corrupt;
                }
            }
        }
    }

    /* Read and verify final EDC */
    if (fread(sector, 1, EDC_SIZE, in) != EDC_SIZE) {
        goto uneof;
    }

    off_t inpos = ftello(in);
    off_t outpos = ftello(out);
    fprintf(stderr, "Decoded %lld bytes -> %lld bytes\n", (long long)((inpos >= 0) ? inpos : 0),
            (long long)((outpos >= 0) ? outpos : 0));

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
    char *cuefilename = NULL;
    FILE *cuefile = NULL;
    int result = 0;

    /* Allocate cue filename (append ".cue") */
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

int main(int argc, char **argv) {
    FILE *fin = NULL;
    FILE *fout = NULL;
    const char *infilename;
    char *outfilename = NULL;
    int outfilename_allocated = 0;
    int createcue = 0;
    int result = 0;
    int argoffset = 0;
    decode_stats_t stats = {0, 0};

    banner();

    /* Initialize the ECC/EDC tables */
    eccedc_init();

    /* Check command line */
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s [--cue] ecmfile [outputfile]\n", argv[0]);
        return 1;
    }

    /* Check for --cue option */
    if (argc >= 2 && strcasecmp(argv[1], "--cue") == 0) {
        createcue = 1;
        argoffset = 1;
    }

    /* Verify we have enough arguments after options */
    if (argc < 2 + argoffset) {
        fprintf(stderr, "usage: %s [--cue] ecmfile [outputfile]\n", argv[0]);
        return 1;
    }

    infilename = argv[1 + argoffset];

    /* Verify input filename ends with .ecm */
    size_t inlen = strlen(infilename);
    if (inlen < 5) {
        fprintf(stderr, "filename '%s' is too short\n", infilename);
        return 1;
    }
    if (strcasecmp(infilename + inlen - 4, ".ecm") != 0) {
        fprintf(stderr, "filename must end in .ecm\n");
        return 1;
    }

    /* Figure out output filename */
    if (argc == 3 + argoffset) {
        outfilename = argv[2 + argoffset];
    } else {
        /* Remove .ecm extension: allocate inlen - 4 + 1 for null terminator */
        outfilename = malloc(inlen - 3);
        if (!outfilename) {
            fprintf(stderr, "Error: failed to allocate output filename\n");
            return 1;
        }
        outfilename_allocated = 1;
        memcpy(outfilename, infilename, inlen - 4);
        outfilename[inlen - 4] = '\0';
    }

    fprintf(stderr, "Decoding %s to %s.\n", infilename, outfilename);

    /* Open input file */
    fin = fopen(infilename, "rb");
    if (!fin) {
        perror(infilename);
        result = 1;
        goto cleanup;
    }

    /* Open output file */
    fout = fopen(outfilename, "wb");
    if (!fout) {
        perror(outfilename);
        result = 1;
        goto cleanup;
    }

    /* Decode */
    result = unecmify(fin, fout, &stats);

    /* Close output file before creating CUE (need complete file) */
    fclose(fout);
    fout = NULL;

    /* Write CUE file if requested */
    if (result == 0 && createcue) {
        result = write_cue_file(outfilename, &stats);
    }

cleanup:
    if (fout) {
        fclose(fout);
    }
    if (fin) {
        fclose(fin);
    }
    if (outfilename_allocated) {
        free(outfilename);
    }

    return result;
}
