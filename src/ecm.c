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
    /* Mode 2 ECC verification needs sector - 0x10, so we need 16 bytes of headroom */
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
 * Sector type detection
 */
static sector_type_t check_type(uint8_t *sector, bool can_be_mode1) {
    bool can_be_mode2_form1 = true;
    bool can_be_mode2_form2 = true;

    /* Check for Mode 1 sync pattern and structure */
    if (can_be_mode1) {
        if (!check_sync_pattern(sector) || sector[OFFSET_MODE] != 0x01 ||
            !check_reserved_zero(sector)) {
            can_be_mode1 = false;
        }
    }

    /* Check for Mode 2 subheader duplication */
    if (!check_subheader_dup(sector)) {
        can_be_mode2_form1 = false;
        can_be_mode2_form2 = false;
        if (!can_be_mode1) {
            return SECTOR_TYPE_LITERAL;
        }
    }

    /* Check EDC for Mode 2 Form 1 */
    uint32_t edc = edc_compute(0, sector, MODE2_EDC_OFFSET);
    if (can_be_mode2_form1 && !check_edc_match(sector, edc, MODE2_EDC_OFFSET)) {
        can_be_mode2_form1 = false;
    }

    /* Check EDC for Mode 1 (continues from Mode 2 Form 1 EDC) */
    edc = edc_compute(edc, sector + MODE2_EDC_OFFSET, 8);
    if (can_be_mode1 && !check_edc_match(sector, edc, OFFSET_MODE1_EDC)) {
        can_be_mode1 = false;
    }

    /* Check EDC for Mode 2 Form 2 */
    edc = edc_compute(edc, sector + OFFSET_MODE1_EDC, 0x10C);
    if (can_be_mode2_form2 && !check_edc_match(sector, edc, MODE2_FORM2_EDC_OFFSET)) {
        can_be_mode2_form2 = false;
    }

    /* Verify ECC */
    if (can_be_mode1 && !ecc_verify(sector, false, sector + OFFSET_MODE1_ECC_P)) {
        can_be_mode1 = false;
    }
    if (can_be_mode2_form1 && !ecc_verify(sector - 0x10, true, sector + 0x80C)) {
        can_be_mode2_form1 = false;
    }

    if (can_be_mode1)
        return SECTOR_TYPE_MODE1;
    if (can_be_mode2_form1)
        return SECTOR_TYPE_MODE2_FORM1;
    if (can_be_mode2_form2)
        return SECTOR_TYPE_MODE2_FORM2;
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
 * Write sector data for a specific type
 */
static int write_sector_data(FILE *in, FILE *out, uint32_t *edc, sector_type_t type,
                             progress_t *progress) {
    uint8_t buf[SECTOR_SIZE_RAW];
    size_t read_size, write_offset, write_size;

    switch (type) {
        case SECTOR_TYPE_MODE1:
            read_size = SECTOR_SIZE_RAW;
            write_offset = OFFSET_HEADER;
            write_size = MODE1_ADDRESS_SIZE;
            break;
        case SECTOR_TYPE_MODE2_FORM1:
            read_size = SECTOR_SIZE_MODE2;
            write_offset = MODE2_SUBHEADER_SIZE;
            write_size = MODE2_FORM1_DATA_SIZE;
            break;
        case SECTOR_TYPE_MODE2_FORM2:
            read_size = SECTOR_SIZE_MODE2;
            write_offset = MODE2_SUBHEADER_SIZE;
            write_size = MODE2_FORM2_DATA_SIZE;
            break;
        default:
            return -1;
    }

    if (fread(buf, 1, read_size, in) != read_size) {
        fprintf(stderr, "Error: unexpected end of input file\n");
        return -1;
    }
    *edc = edc_compute(*edc, buf, read_size);

    if (type == SECTOR_TYPE_MODE1) {
        /* Mode 1: write address + user data separately */
        if (fwrite(buf + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, out) != MODE1_ADDRESS_SIZE ||
            fwrite(buf + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, out) != SECTOR_USER_DATA) {
            fprintf(stderr, "Error: failed to write output\n");
            return -1;
        }
    } else {
        if (fwrite(buf + write_offset, 1, write_size, out) != write_size) {
            fprintf(stderr, "Error: failed to write output\n");
            return -1;
        }
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
 * Get sector size for type
 */
static size_t sector_size_for_type(sector_type_t type) {
    switch (type) {
        case SECTOR_TYPE_MODE1:
            return SECTOR_SIZE_RAW;
        case SECTOR_TYPE_MODE2_FORM1:
        case SECTOR_TYPE_MODE2_FORM2:
            return SECTOR_SIZE_MODE2;
        default:
            return 1;
    }
}

/*
 * Main encoding function
 */
static int ecmify(FILE *in, FILE *out) {
    uint8_t *inputqueue = nullptr;
    uint32_t inedc = 0;
    sector_type_t curtype = SECTOR_TYPE_LITERAL;
    int curtypecount = 0;
    int64_t curtype_in_start = 0;
    int64_t incheckpos = 0;
    int64_t inbufferpos = 0;
    int64_t intotallength;
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
    progress_reset(&progress, intotallength);

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

        /* Detect sector type */
        sector_type_t detecttype;
        if (dataavail < SECTOR_SIZE_MODE2) {
            detecttype = SECTOR_TYPE_LITERAL;
        } else {
            detecttype = check_type(inputqueue + INPUT_QUEUE_PADDING + inqueuestart,
                                    dataavail >= SECTOR_SIZE_RAW);
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
            curtypecount = 1;
        } else {
            if (first_run) {
                curtype = detecttype;
                curtype_in_start = incheckpos;
                first_run = false;
            }
            curtypecount++;
        }

        /* Advance position */
        size_t step = sector_size_for_type(curtype);
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

    result = ecmify(fin, fout);

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
