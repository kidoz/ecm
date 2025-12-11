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

enum {
    /* Extra buffer space for alignment */
    INPUT_QUEUE_PADDING = 0x10,
    INPUT_QUEUE_SIZE = 1048576 + INPUT_QUEUE_PADDING
};

static void banner(void) {
    fprintf(stderr, "ECM - Encoder for Error Code Modeler format v" ECM_VERSION "\n\n");
}

/*
 * Verify sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00
 */
static bool check_sync_pattern(const uint8_t *sector) {
    if (sector[0x00] != SYNC_BYTE_START)
        return false;
    for (int i = 1; i <= 10; i++) {
        if (sector[i] != SYNC_BYTE_MIDDLE)
            return false;
    }
    return sector[0x0B] == SYNC_BYTE_END;
}

/*
 * Verify reserved bytes are zero
 */
static bool check_reserved_zero(const uint8_t *sector) {
    for (int i = 0; i < RESERVED_SIZE; i++) {
        if (sector[OFFSET_MODE1_RESERVED + i] != 0)
            return false;
    }
    return true;
}

/*
 * Verify subheader duplication (bytes 0-3 == bytes 4-7)
 */
static bool check_subheader_dup(const uint8_t *sector) {
    return sector[0] == sector[4] && sector[1] == sector[5] && sector[2] == sector[6] &&
           sector[3] == sector[7];
}

/*
 * Verify EDC matches at given offset
 */
static bool check_edc_match(const uint8_t *sector, uint32_t edc, int offset) {
    return sector[offset + 0] == ((edc >> 0) & 0xFF) && sector[offset + 1] == ((edc >> 8) & 0xFF) &&
           sector[offset + 2] == ((edc >> 16) & 0xFF) && sector[offset + 3] == ((edc >> 24) & 0xFF);
}

/*
 * Sector type detection for raw 2352-byte sectors (with sync/header)
 */
static sector_type_t check_type_raw(uint8_t *sector) {
    bool can_be_mode1 = true;
    bool can_be_mode2_form1 = true;
    bool can_be_mode2_form2 = true;

    /* All raw sectors must have sync pattern */
    if (!check_sync_pattern(sector)) {
        return SECTOR_TYPE_LITERAL;
    }

    /* Check mode byte to determine sector type */
    uint8_t mode = sector[OFFSET_MODE];
    if (mode == 0x01) {
        /* Mode 1 sector */
        can_be_mode2_form1 = false;
        can_be_mode2_form2 = false;
        if (!check_reserved_zero(sector)) {
            can_be_mode1 = false;
        }
    } else if (mode == 0x02) {
        /* Mode 2 sector - check subheader at offset 0x10 */
        can_be_mode1 = false;
        if (!check_subheader_dup(sector + OFFSET_MODE2_SUBHEADER)) {
            return SECTOR_TYPE_LITERAL;
        }
    } else {
        return SECTOR_TYPE_LITERAL;
    }

    /* EDC/ECC verification for Mode 1 */
    if (can_be_mode1) {
        uint32_t edc = edc_compute(0, sector, OFFSET_MODE1_EDC);
        if (!check_edc_match(sector, edc, OFFSET_MODE1_EDC)) {
            return SECTOR_TYPE_LITERAL;
        }
        if (!ecc_verify(sector, false, sector + OFFSET_MODE1_ECC_P)) {
            return SECTOR_TYPE_LITERAL;
        }
        return SECTOR_TYPE_MODE1;
    }

    /* EDC/ECC verification for Mode 2 Form 1 */
    if (can_be_mode2_form1) {
        uint32_t edc = edc_compute(0, sector + OFFSET_MODE2_SUBHEADER, MODE2_EDC_OFFSET);
        if (check_edc_match(sector + OFFSET_MODE2_SUBHEADER, edc, MODE2_EDC_OFFSET)) {
            if (ecc_verify(sector, true, sector + 0x81C)) {
                return SECTOR_TYPE_MODE2_FORM1;
            }
        }
        can_be_mode2_form1 = false;
    }

    /* EDC verification for Mode 2 Form 2 */
    if (can_be_mode2_form2) {
        uint32_t edc = edc_compute(0, sector + OFFSET_MODE2_SUBHEADER, MODE2_FORM2_EDC_OFFSET);
        if (check_edc_match(sector + OFFSET_MODE2_SUBHEADER, edc, MODE2_FORM2_EDC_OFFSET)) {
            return SECTOR_TYPE_MODE2_FORM2;
        }
    }

    return SECTOR_TYPE_LITERAL;
}


/*
 * Encode a type/count combo to output
 */
[[nodiscard]] static int write_type_count(FILE *out, unsigned type, unsigned count) {
    count--;
    if (fputc(((count >= 32) << 7) | ((count & 31) << 2) | type, out) == EOF) {
        return -1;
    }
    count >>= 5;
    while (count) {
        if (fputc(((count >= 128) << 7) | (count & 127), out) == EOF) {
            return -1;
        }
        count >>= 7;
    }
    return 0;
}

/*
 * Progress tracking
 */
typedef struct {
    int64_t analyze;
    int64_t encode;
    int64_t total;
} progress_t;

static void progress_reset(progress_t *p, int64_t total) {
    p->analyze = 0;
    p->encode = 0;
    p->total = total;
}

static void progress_update(progress_t *p, int64_t analyze, int64_t encode) {
    bool changed = (analyze >> 20) != (p->analyze >> 20) || (encode >> 20) != (p->encode >> 20);
    p->analyze = analyze;
    p->encode = encode;

    if (changed) {
        int64_t d = (p->total + 64) / 128;
        if (!d)
            d = 1;
        fprintf(stderr, "Analyzing (%02u%%) Encoding (%02u%%)\r",
                (unsigned)((100 * ((analyze + 64) / 128)) / d),
                (unsigned)((100 * ((encode + 64) / 128)) / d));
    }
}

/*
 * Write sector data for a specific type.
 * All sectors are raw 2352-byte format.
 */
static int write_sector_data(FILE *in, FILE *out, uint32_t *edc, sector_type_t type,
                             progress_t *progress) {
    uint8_t buf[SECTOR_SIZE_RAW];

    /* Always read full 2352-byte raw sector */
    if (fread(buf, 1, SECTOR_SIZE_RAW, in) != SECTOR_SIZE_RAW) {
        fprintf(stderr, "Error: unexpected end of input file\n");
        return -1;
    }

    switch (type) {
        case SECTOR_TYPE_MODE1:
            /* EDC over full 2352 bytes */
            *edc = edc_compute(*edc, buf, SECTOR_SIZE_RAW);
            /* Write address (3 bytes) + user data (2048 bytes) */
            if (fwrite(buf + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, out) != MODE1_ADDRESS_SIZE ||
                fwrite(buf + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, out) != SECTOR_USER_DATA) {
                fprintf(stderr, "Error: failed to write output\n");
                return -1;
            }
            break;

        case SECTOR_TYPE_MODE2_FORM1:
            /* EDC over 2336 bytes (Mode 2 format, offset 0x10-0x92F) for compatibility */
            *edc = edc_compute(*edc, buf + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
            /* Write duplicated subheader (4 bytes) + user data (2048 bytes) */
            if (fwrite(buf + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM1_DATA_SIZE,
                       out) != MODE2_FORM1_DATA_SIZE) {
                fprintf(stderr, "Error: failed to write output\n");
                return -1;
            }
            break;

        case SECTOR_TYPE_MODE2_FORM2:
            /* EDC over 2336 bytes (Mode 2 format, offset 0x10-0x92F) for compatibility */
            *edc = edc_compute(*edc, buf + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
            /* Write duplicated subheader (4 bytes) + user data (2324 bytes) */
            if (fwrite(buf + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM2_DATA_SIZE,
                       out) != MODE2_FORM2_DATA_SIZE) {
                fprintf(stderr, "Error: failed to write output\n");
                return -1;
            }
            break;

        default:
            return -1;
    }

    off_t pos = ftello(in);
    if (pos >= 0) {
        progress_update(progress, progress->analyze, pos);
    }
    return 0;
}

/*
 * Encode a run of sectors/literals of the same type
 */
static int flush_sector_run(uint32_t *edc, sector_type_t type, unsigned count, FILE *in, FILE *out,
                            progress_t *progress) {
    uint8_t buf[SECTOR_SIZE_RAW];

    if (write_type_count(out, type, count) < 0) {
        fprintf(stderr, "Error: failed to write output\n");
        return -1;
    }

    if (type == SECTOR_TYPE_LITERAL) {
        while (count) {
            unsigned b = (count > SECTOR_SIZE_RAW) ? SECTOR_SIZE_RAW : count;
            if (fread(buf, 1, b, in) != b) {
                fprintf(stderr, "Error: unexpected end of input file\n");
                return -1;
            }
            *edc = edc_compute(*edc, buf, b);
            if (fwrite(buf, 1, b, out) != b) {
                fprintf(stderr, "Error: failed to write output\n");
                return -1;
            }
            count -= b;
            off_t pos = ftello(in);
            if (pos >= 0) {
                progress_update(progress, progress->analyze, pos);
            }
        }
        return 0;
    }

    while (count--) {
        if (write_sector_data(in, out, edc, type, progress) < 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Get sector size for type.
 * All sector types (Mode 1, Mode 2) use raw 2352-byte format.
 */
static size_t sector_size_for_type(sector_type_t type) {
    switch (type) {
        case SECTOR_TYPE_LITERAL:
            return SECTOR_SIZE_RAW;
        case SECTOR_TYPE_MODE1:
        case SECTOR_TYPE_MODE2_FORM1:
        case SECTOR_TYPE_MODE2_FORM2:
            return SECTOR_SIZE_RAW;
        default:
            return 1;
    }
}

/*
 * Encode sector data from a buffer (for streaming mode).
 */
static int encode_sector_from_buffer(const uint8_t *buf, FILE *out, uint32_t *edc,
                                     sector_type_t type) {
    switch (type) {
        case SECTOR_TYPE_MODE1:
            /* EDC over full 2352 bytes */
            *edc = edc_compute(*edc, buf, SECTOR_SIZE_RAW);
            /* Write address (3 bytes) + user data (2048 bytes) */
            if (fwrite(buf + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, out) != MODE1_ADDRESS_SIZE ||
                fwrite(buf + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, out) != SECTOR_USER_DATA) {
                return -1;
            }
            break;

        case SECTOR_TYPE_MODE2_FORM1:
            /* EDC over 2336 bytes (Mode 2 format) for compatibility */
            *edc = edc_compute(*edc, buf + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
            /* Write duplicated subheader (4 bytes) + user data (2048 bytes) */
            if (fwrite(buf + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM1_DATA_SIZE,
                       out) != MODE2_FORM1_DATA_SIZE) {
                return -1;
            }
            break;

        case SECTOR_TYPE_MODE2_FORM2:
            /* EDC over 2336 bytes (Mode 2 format) for compatibility */
            *edc = edc_compute(*edc, buf + OFFSET_MODE2_SUBHEADER, SECTOR_SIZE_MODE2);
            /* Write duplicated subheader (4 bytes) + user data (2324 bytes) */
            if (fwrite(buf + OFFSET_MODE2_SUBHEADER + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM2_DATA_SIZE,
                       out) != MODE2_FORM2_DATA_SIZE) {
                return -1;
            }
            break;

        default:
            return -1;
    }
    return 0;
}

/*
 * Streaming mode encoder - processes input without seeking.
 * Less efficient than batch mode but works with stdin/pipes.
 */
static int ecmify_streaming(FILE *in, FILE *out) {
    uint8_t buf[SECTOR_SIZE_RAW + INPUT_QUEUE_PADDING];
    uint32_t inedc = 0;
    sector_type_t curtype = SECTOR_TYPE_LITERAL;
    unsigned curtypecount = 0;
    unsigned typetally[4] = {0};
    int64_t total_in = 0;
    size_t dataavail = 0;

    /* Write magic header */
    if (fputc(ECM_MAGIC_E, out) == EOF || fputc(ECM_MAGIC_C, out) == EOF ||
        fputc(ECM_MAGIC_M, out) == EOF || fputc(ECM_MAGIC_NULL, out) == EOF) {
        fprintf(stderr, "Error: failed to write magic header\n");
        return 1;
    }

    /* Read first chunk */
    dataavail = fread(buf + INPUT_QUEUE_PADDING, 1, SECTOR_SIZE_RAW, in);
    total_in += (int64_t)dataavail;

    while (dataavail > 0) {
        sector_type_t detecttype;

        /* Detect sector type */
        if (dataavail < SECTOR_SIZE_RAW) {
            detecttype = SECTOR_TYPE_LITERAL;
        } else {
            detecttype = check_type_raw(buf + INPUT_QUEUE_PADDING);
        }

        /* Handle type change - flush previous run */
        if (curtypecount > 0 && detecttype != curtype) {
            if (write_type_count(out, curtype, curtypecount) < 0) {
                fprintf(stderr, "Error: failed to write output\n");
                return 1;
            }
            curtypecount = 0;
        }

        /* Handle current sector/data */
        if (detecttype == SECTOR_TYPE_LITERAL) {
            /* Flush any pending run first */
            if (curtypecount > 0) {
                if (write_type_count(out, curtype, curtypecount) < 0) {
                    fprintf(stderr, "Error: failed to write output\n");
                    return 1;
                }
                curtypecount = 0;
            }
            /* Write literal data one byte at a time for now */
            if (write_type_count(out, SECTOR_TYPE_LITERAL, (unsigned)dataavail) < 0) {
                fprintf(stderr, "Error: failed to write output\n");
                return 1;
            }
            inedc = edc_compute(inedc, buf + INPUT_QUEUE_PADDING, dataavail);
            if (fwrite(buf + INPUT_QUEUE_PADDING, 1, dataavail, out) != dataavail) {
                fprintf(stderr, "Error: failed to write output\n");
                return 1;
            }
            typetally[SECTOR_TYPE_LITERAL] += (unsigned)dataavail;
            dataavail = 0;
        } else {
            /* Sector type - accumulate run and encode */
            curtype = detecttype;
            curtypecount++;
            typetally[curtype]++;

            /* For simplicity in streaming mode, flush each sector immediately */
            if (write_type_count(out, curtype, 1) < 0) {
                fprintf(stderr, "Error: failed to write output\n");
                return 1;
            }
            if (encode_sector_from_buffer(buf + INPUT_QUEUE_PADDING, out, &inedc, curtype) < 0) {
                fprintf(stderr, "Error: failed to write output\n");
                return 1;
            }
            curtypecount = 0;

            /* Advance past this sector */
            dataavail = 0;
        }

        /* Read next chunk */
        dataavail = fread(buf + INPUT_QUEUE_PADDING, 1, SECTOR_SIZE_RAW, in);
        total_in += (int64_t)dataavail;
    }

    /* End-of-records indicator */
    if (write_type_count(out, 0, 0) < 0) {
        fprintf(stderr, "Error: failed to write end marker\n");
        return 1;
    }

    /* Write EDC checksum */
    if (fputc((inedc >> 0) & 0xFF, out) == EOF || fputc((inedc >> 8) & 0xFF, out) == EOF ||
        fputc((inedc >> 16) & 0xFF, out) == EOF || fputc((inedc >> 24) & 0xFF, out) == EOF) {
        fprintf(stderr, "Error: failed to write EDC checksum\n");
        return 1;
    }

    /* Show report */
    fprintf(stderr, "Literal bytes........... %10u\n", typetally[SECTOR_TYPE_LITERAL]);
    fprintf(stderr, "Mode 1 sectors.......... %10u\n", typetally[SECTOR_TYPE_MODE1]);
    fprintf(stderr, "Mode 2 form 1 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM1]);
    fprintf(stderr, "Mode 2 form 2 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM2]);
    off_t outpos = ftello(out);
    fprintf(stderr, "Encoded %lld bytes -> %lld bytes\n", (long long)total_in,
            (long long)(outpos >= 0 ? outpos : 0));
    fprintf(stderr, "Done.\n");

    return 0;
}

/*
 * Main encoding function
 */
static int ecmify(FILE *in, FILE *out, bool is_stdin) {
    uint8_t *inputqueue = nullptr;
    uint32_t inedc = 0;
    sector_type_t curtype = SECTOR_TYPE_LITERAL;
    int curtypecount = 0;
    int64_t curtype_in_start = 0;
    int64_t incheckpos = 0;
    int64_t inbufferpos = 0;
    int64_t intotallength = -1; /* -1 means unknown (streaming mode) */
    size_t inqueuestart = 0;
    size_t dataavail = 0;
    unsigned typetally[4] = {0};
    progress_t progress;
    int result = 0;
    bool first_run = true;

    inputqueue = malloc(INPUT_QUEUE_SIZE);
    if (!inputqueue) {
        fprintf(stderr, "Error: failed to allocate input buffer\n");
        return 1;
    }

    /* For regular files, get size for progress tracking; for stdin, use streaming mode */
    if (!is_stdin) {
        if (fseeko(in, 0, SEEK_END) != 0) {
            fprintf(stderr, "Error: failed to seek input file\n");
            free(inputqueue);
            return 1;
        }
        off_t endpos = ftello(in);
        if (endpos < 0) {
            fprintf(stderr, "Error: failed to determine input file size\n");
            free(inputqueue);
            return 1;
        }
        intotallength = (int64_t)endpos;
        if (fseeko(in, 0, SEEK_SET) != 0) {
            fprintf(stderr, "Error: failed to rewind input file\n");
            free(inputqueue);
            return 1;
        }
    }
    progress_reset(&progress, intotallength > 0 ? intotallength : 0);

    /* Write magic header */
    if (fputc(ECM_MAGIC_E, out) == EOF || fputc(ECM_MAGIC_C, out) == EOF ||
        fputc(ECM_MAGIC_M, out) == EOF || fputc(ECM_MAGIC_NULL, out) == EOF) {
        fprintf(stderr, "Error: failed to write magic header\n");
        free(inputqueue);
        return 1;
    }

    for (;;) {
        /* Fill input buffer if needed */
        if (dataavail < SECTOR_SIZE_RAW && (int64_t)dataavail < intotallength - inbufferpos) {
            size_t willread = (size_t)(intotallength - inbufferpos);
            if (willread > (INPUT_QUEUE_SIZE - INPUT_QUEUE_PADDING) - dataavail) {
                willread = (INPUT_QUEUE_SIZE - INPUT_QUEUE_PADDING) - dataavail;
            }
            if (inqueuestart) {
                memmove(inputqueue + INPUT_QUEUE_PADDING,
                        inputqueue + INPUT_QUEUE_PADDING + inqueuestart, dataavail);
                inqueuestart = 0;
            }
            if (willread) {
                progress_update(&progress, inbufferpos, progress.encode);
                if (fseeko(in, (off_t)inbufferpos, SEEK_SET) != 0) {
                    fprintf(stderr, "Error: failed to seek input file\n");
                    result = 1;
                    goto cleanup;
                }
                if (fread(inputqueue + INPUT_QUEUE_PADDING + dataavail, 1, willread, in) != willread) {
                    fprintf(stderr, "Error: failed to read input file\n");
                    result = 1;
                    goto cleanup;
                }
                inbufferpos += (int64_t)willread;
                dataavail += willread;
            }
        }

        if (dataavail == 0)
            break;

        /* Detect sector type - all sectors require 2352 bytes (raw format) */
        sector_type_t detecttype;
        if (dataavail < SECTOR_SIZE_RAW) {
            detecttype = SECTOR_TYPE_LITERAL;
        } else {
            detecttype = check_type_raw(inputqueue + INPUT_QUEUE_PADDING + inqueuestart);
        }

        /* Flush previous run if type changed */
        if (!first_run && detecttype != curtype) {
            if (curtypecount) {
                if (fseeko(in, (off_t)curtype_in_start, SEEK_SET) != 0) {
                    fprintf(stderr, "Error: failed to seek input file\n");
                    result = 1;
                    goto cleanup;
                }
                typetally[curtype] += (unsigned)curtypecount;
                if (flush_sector_run(&inedc, curtype, (unsigned)curtypecount, in, out, &progress) <
                    0) {
                    result = 1;
                    goto cleanup;
                }
            }
            curtype = detecttype;
            curtype_in_start = incheckpos;
            curtypecount = 0;
        } else {
            if (first_run) {
                curtype = detecttype;
                curtype_in_start = incheckpos;
                first_run = false;
            }
        }

        /* Determine how far to advance and tally counts */
        size_t step;
        if (curtype == SECTOR_TYPE_LITERAL) {
            step = (dataavail >= SECTOR_SIZE_RAW) ? SECTOR_SIZE_RAW : dataavail;
            if (step == 0) {
                step = 1; /* Safety to avoid infinite loop */
            }
            curtypecount += (int)step;
        } else {
            step = sector_size_for_type(curtype);
            curtypecount++;
        }

        /* Advance position */
        incheckpos += (int64_t)step;
        inqueuestart += step;
        dataavail -= step;
    }

    /* Flush final run */
    if (curtypecount) {
        if (fseeko(in, (off_t)curtype_in_start, SEEK_SET) != 0) {
            fprintf(stderr, "Error: failed to seek input file\n");
            result = 1;
            goto cleanup;
        }
        typetally[curtype] += (unsigned)curtypecount;
        if (flush_sector_run(&inedc, curtype, (unsigned)curtypecount, in, out, &progress) < 0) {
            result = 1;
            goto cleanup;
        }
    }

    /* End-of-records indicator */
    if (write_type_count(out, 0, 0) < 0) {
        fprintf(stderr, "Error: failed to write end marker\n");
        result = 1;
        goto cleanup;
    }

    /* Write EDC checksum */
    if (fputc((inedc >> 0) & 0xFF, out) == EOF || fputc((inedc >> 8) & 0xFF, out) == EOF ||
        fputc((inedc >> 16) & 0xFF, out) == EOF || fputc((inedc >> 24) & 0xFF, out) == EOF) {
        fprintf(stderr, "Error: failed to write EDC checksum\n");
        result = 1;
        goto cleanup;
    }

    /* Show report */
    fprintf(stderr, "Literal bytes........... %10u\n", typetally[SECTOR_TYPE_LITERAL]);
    fprintf(stderr, "Mode 1 sectors.......... %10u\n", typetally[SECTOR_TYPE_MODE1]);
    fprintf(stderr, "Mode 2 form 1 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM1]);
    fprintf(stderr, "Mode 2 form 2 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM2]);
    off_t outpos = ftello(out);
    fprintf(stderr, "Encoded %lld bytes -> %lld bytes\n", (long long)intotallength,
            (long long)(outpos >= 0 ? outpos : 0));
    fprintf(stderr, "Done.\n");

cleanup:
    free(inputqueue);
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
    int result = 0;

    banner();
    eccedc_init();

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s cdimagefile [ecmfile]\n", argv[0]);
        fprintf(stderr, "       use '-' for stdin/stdout\n");
        return 1;
    }

    infilename = argv[1];

    /* Determine output filename */
    if (argc == 3) {
        outfilename = argv[2];
    } else if (is_stdio(infilename)) {
        outfilename = "-";
    } else {
        size_t len = strlen(infilename);
        outfilename = malloc(len + 5);
        if (!outfilename) {
            fprintf(stderr, "Error: failed to allocate output filename\n");
            return 1;
        }
        outfilename_allocated = true;
        snprintf(outfilename, len + 5, "%s.ecm", infilename);
    }

    fprintf(stderr, "Encoding %s to %s.\n", infilename, outfilename);

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

    /* Use streaming mode for stdin, batch mode for regular files */
    if (is_stdio(infilename)) {
        result = ecmify_streaming(fin, fout);
    } else {
        result = ecmify(fin, fout, false);
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
