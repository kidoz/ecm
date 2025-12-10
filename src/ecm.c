#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "eccedc.h"
#include "version.h"

#define INPUT_QUEUE_SIZE (1048576 + 4)

static void banner(void) {
    fprintf(stderr, "ECM - Encoder for Error Code Modeler format v" ECM_VERSION "\n\n");
}

/*
 * Sector type detection
 *
 * Returns:
 *   SECTOR_TYPE_LITERAL     - literal bytes (no pattern detected)
 *   SECTOR_TYPE_MODE1       - 2352 byte Mode 1 sector
 *   SECTOR_TYPE_MODE2_FORM1 - 2336 byte Mode 2 Form 1 sector
 *   SECTOR_TYPE_MODE2_FORM2 - 2336 byte Mode 2 Form 2 sector
 */
static int check_type(uint8_t *sector, int canbetype1) {
    int canbetype2 = 1;
    int canbetype3 = 1;
    uint32_t myedc;

    /* Check for Mode 1 sync pattern and structure */
    if (canbetype1) {
        if (sector[0x00] != SYNC_BYTE_START ||
            sector[0x01] != SYNC_BYTE_MIDDLE ||
            sector[0x02] != SYNC_BYTE_MIDDLE ||
            sector[0x03] != SYNC_BYTE_MIDDLE ||
            sector[0x04] != SYNC_BYTE_MIDDLE ||
            sector[0x05] != SYNC_BYTE_MIDDLE ||
            sector[0x06] != SYNC_BYTE_MIDDLE ||
            sector[0x07] != SYNC_BYTE_MIDDLE ||
            sector[0x08] != SYNC_BYTE_MIDDLE ||
            sector[0x09] != SYNC_BYTE_MIDDLE ||
            sector[0x0A] != SYNC_BYTE_MIDDLE ||
            sector[0x0B] != SYNC_BYTE_END ||
            sector[OFFSET_MODE] != 0x01 ||
            sector[OFFSET_MODE1_RESERVED + 0] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 1] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 2] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 3] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 4] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 5] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 6] != 0x00 ||
            sector[OFFSET_MODE1_RESERVED + 7] != 0x00) {
            canbetype1 = 0;
        }
    }

    /* Check for Mode 2 subheader duplication */
    if (sector[0x0] != sector[0x4] ||
        sector[0x1] != sector[0x5] ||
        sector[0x2] != sector[0x6] ||
        sector[0x3] != sector[0x7]) {
        canbetype2 = 0;
        canbetype3 = 0;
        if (!canbetype1) {
            return SECTOR_TYPE_LITERAL;
        }
    }

    /* Check EDC for Mode 2 Form 1 */
    myedc = edc_compute(0, sector, 0x808);
    if (canbetype2) {
        if (sector[0x808] != ((myedc >> 0) & 0xFF) ||
            sector[0x809] != ((myedc >> 8) & 0xFF) ||
            sector[0x80A] != ((myedc >> 16) & 0xFF) ||
            sector[0x80B] != ((myedc >> 24) & 0xFF)) {
            canbetype2 = 0;
        }
    }

    /* Check EDC for Mode 1 */
    myedc = edc_compute(myedc, sector + 0x808, 8);
    if (canbetype1) {
        if (sector[OFFSET_MODE1_EDC + 0] != ((myedc >> 0) & 0xFF) ||
            sector[OFFSET_MODE1_EDC + 1] != ((myedc >> 8) & 0xFF) ||
            sector[OFFSET_MODE1_EDC + 2] != ((myedc >> 16) & 0xFF) ||
            sector[OFFSET_MODE1_EDC + 3] != ((myedc >> 24) & 0xFF)) {
            canbetype1 = 0;
        }
    }

    /* Check EDC for Mode 2 Form 2 */
    myedc = edc_compute(myedc, sector + OFFSET_MODE1_EDC, 0x10C);
    if (canbetype3) {
        if (sector[0x91C] != ((myedc >> 0) & 0xFF) ||
            sector[0x91D] != ((myedc >> 8) & 0xFF) ||
            sector[0x91E] != ((myedc >> 16) & 0xFF) ||
            sector[0x91F] != ((myedc >> 24) & 0xFF)) {
            canbetype3 = 0;
        }
    }

    /* Verify ECC */
    if (canbetype1) {
        if (!ecc_verify(sector, 0, sector + OFFSET_MODE1_ECC_P)) {
            canbetype1 = 0;
        }
    }
    if (canbetype2) {
        if (!ecc_verify(sector - 0x10, 1, sector + 0x80C)) {
            canbetype2 = 0;
        }
    }

    if (canbetype1) return SECTOR_TYPE_MODE1;
    if (canbetype2) return SECTOR_TYPE_MODE2_FORM1;
    if (canbetype3) return SECTOR_TYPE_MODE2_FORM2;
    return SECTOR_TYPE_LITERAL;
}

/*
 * Encode a type/count combo to output
 */
static void write_type_count(FILE *out, unsigned type, unsigned count) {
    count--;
    fputc(((count >= 32) << 7) | ((count & 31) << 2) | type, out);
    count >>= 5;
    while (count) {
        fputc(((count >= 128) << 7) | (count & 127), out);
        count >>= 7;
    }
}

/*
 * Progress tracking (using int64_t for large file support)
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

static void progress_set_analyze(progress_t *p, int64_t n) {
    if ((n >> 20) != (p->analyze >> 20)) {
        int64_t a = (n + 64) / 128;
        int64_t e = (p->encode + 64) / 128;
        int64_t d = (p->total + 64) / 128;
        if (!d) d = 1;
        fprintf(stderr, "Analyzing (%02u%%) Encoding (%02u%%)\r",
            (unsigned)((100 * a) / d), (unsigned)((100 * e) / d));
    }
    p->analyze = n;
}

static void progress_set_encode(progress_t *p, int64_t n) {
    if ((n >> 20) != (p->encode >> 20)) {
        int64_t a = (p->analyze + 64) / 128;
        int64_t e = (n + 64) / 128;
        int64_t d = (p->total + 64) / 128;
        if (!d) d = 1;
        fprintf(stderr, "Analyzing (%02u%%) Encoding (%02u%%)\r",
            (unsigned)((100 * a) / d), (unsigned)((100 * e) / d));
    }
    p->encode = n;
}

/*
 * Encode a run of sectors/literals of the same type
 * Returns 0 on success, -1 on error. Updates *edc with running checksum.
 */
static int flush_sector_run(
    uint32_t *edc,
    unsigned type,
    unsigned count,
    FILE *in,
    FILE *out,
    progress_t *progress
) {
    uint8_t buf[SECTOR_SIZE_RAW];
    size_t bytes_read;

    write_type_count(out, type, count);

    if (type == SECTOR_TYPE_LITERAL) {
        while (count) {
            unsigned b = count;
            if (b > SECTOR_SIZE_RAW) {
                b = SECTOR_SIZE_RAW;
            }
            bytes_read = fread(buf, 1, b, in);
            if (bytes_read != b) {
                fprintf(stderr, "Error: unexpected end of input file\n");
                return -1;
            }
            *edc = edc_compute(*edc, buf, b);
            if (fwrite(buf, 1, b, out) != b) {
                fprintf(stderr, "Error: failed to write output\n");
                return -1;
            }
            count -= b;
            progress_set_encode(progress, ftell(in));
        }
        return 0;
    }

    while (count--) {
        switch (type) {
            case SECTOR_TYPE_MODE1:
                bytes_read = fread(buf, 1, SECTOR_SIZE_RAW, in);
                if (bytes_read != SECTOR_SIZE_RAW) {
                    fprintf(stderr, "Error: unexpected end of input file\n");
                    return -1;
                }
                *edc = edc_compute(*edc, buf, SECTOR_SIZE_RAW);
                /* Write address (3 bytes) and user data (2048 bytes) */
                if (fwrite(buf + OFFSET_HEADER, 1, MODE1_ADDRESS_SIZE, out) != MODE1_ADDRESS_SIZE ||
                    fwrite(buf + OFFSET_MODE1_DATA, 1, SECTOR_USER_DATA, out) != SECTOR_USER_DATA) {
                    fprintf(stderr, "Error: failed to write output\n");
                    return -1;
                }
                progress_set_encode(progress, ftell(in));
                break;

            case SECTOR_TYPE_MODE2_FORM1:
                bytes_read = fread(buf, 1, SECTOR_SIZE_MODE2, in);
                if (bytes_read != SECTOR_SIZE_MODE2) {
                    fprintf(stderr, "Error: unexpected end of input file\n");
                    return -1;
                }
                *edc = edc_compute(*edc, buf, SECTOR_SIZE_MODE2);
                /* Write subheader (4 bytes) and user data (2048 bytes) */
                if (fwrite(buf + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM1_DATA_SIZE, out) != MODE2_FORM1_DATA_SIZE) {
                    fprintf(stderr, "Error: failed to write output\n");
                    return -1;
                }
                progress_set_encode(progress, ftell(in));
                break;

            case SECTOR_TYPE_MODE2_FORM2:
                bytes_read = fread(buf, 1, SECTOR_SIZE_MODE2, in);
                if (bytes_read != SECTOR_SIZE_MODE2) {
                    fprintf(stderr, "Error: unexpected end of input file\n");
                    return -1;
                }
                *edc = edc_compute(*edc, buf, SECTOR_SIZE_MODE2);
                /* Write subheader and data (2328 bytes total) */
                if (fwrite(buf + MODE2_SUBHEADER_SIZE, 1, MODE2_FORM2_DATA_SIZE, out) != MODE2_FORM2_DATA_SIZE) {
                    fprintf(stderr, "Error: failed to write output\n");
                    return -1;
                }
                progress_set_encode(progress, ftell(in));
                break;
        }
    }
    return 0;
}

/*
 * Main encoding function
 */
static int ecmify(FILE *in, FILE *out) {
    uint8_t *inputqueue = NULL;
    uint32_t inedc = 0;
    int curtype = -1;
    int curtypecount = 0;
    int64_t curtype_in_start = 0;
    int detecttype;
    int64_t incheckpos = 0;
    int64_t inbufferpos = 0;
    int64_t intotallength;
    int inqueuestart = 0;
    int dataavail = 0;
    unsigned typetally[4] = {0, 0, 0, 0};
    progress_t progress;
    int result = 0;

    /* Allocate input queue */
    inputqueue = malloc(INPUT_QUEUE_SIZE);
    if (!inputqueue) {
        fprintf(stderr, "Error: failed to allocate input buffer\n");
        return 1;
    }

    fseek(in, 0, SEEK_END);
    intotallength = ftell(in);
    progress_reset(&progress, intotallength);

    /* Write magic header */
    fputc(ECM_MAGIC_E, out);
    fputc(ECM_MAGIC_C, out);
    fputc(ECM_MAGIC_M, out);
    fputc(ECM_MAGIC_NULL, out);

    for (;;) {
        if ((dataavail < SECTOR_SIZE_RAW) && (dataavail < (intotallength - inbufferpos))) {
            int64_t willread = intotallength - inbufferpos;
            if (willread > (int64_t)((INPUT_QUEUE_SIZE - 4) - (size_t)dataavail)) {
                willread = (int64_t)((INPUT_QUEUE_SIZE - 4) - (size_t)dataavail);
            }
            if (inqueuestart) {
                memmove(inputqueue + 4, inputqueue + 4 + inqueuestart, (size_t)dataavail);
                inqueuestart = 0;
            }
            if (willread) {
                progress_set_analyze(&progress, inbufferpos);
                fseek(in, (long)inbufferpos, SEEK_SET);
                size_t bytes_read = fread(inputqueue + 4 + dataavail, 1, (size_t)willread, in);
                if ((int64_t)bytes_read != willread) {
                    fprintf(stderr, "Warning: read fewer bytes than expected\n");
                    willread = (int64_t)bytes_read;
                }
                inbufferpos += willread;
                dataavail += (int)willread;
            }
        }

        if (dataavail <= 0) {
            break;
        }

        if (dataavail < SECTOR_SIZE_MODE2) {
            detecttype = SECTOR_TYPE_LITERAL;
        } else {
            detecttype = check_type(inputqueue + 4 + inqueuestart, dataavail >= SECTOR_SIZE_RAW);
        }

        if (detecttype != curtype) {
            if (curtypecount) {
                fseek(in, (long)curtype_in_start, SEEK_SET);
                typetally[curtype] += (unsigned)curtypecount;
                if (flush_sector_run(&inedc, (unsigned)curtype, (unsigned)curtypecount, in, out, &progress) < 0) {
                    result = 1;
                    goto cleanup;
                }
            }
            curtype = detecttype;
            curtype_in_start = incheckpos;
            curtypecount = 1;
        } else {
            curtypecount++;
        }

        switch (curtype) {
            case SECTOR_TYPE_LITERAL:
                incheckpos += 1;
                inqueuestart += 1;
                dataavail -= 1;
                break;
            case SECTOR_TYPE_MODE1:
                incheckpos += SECTOR_SIZE_RAW;
                inqueuestart += SECTOR_SIZE_RAW;
                dataavail -= SECTOR_SIZE_RAW;
                break;
            case SECTOR_TYPE_MODE2_FORM1:
            case SECTOR_TYPE_MODE2_FORM2:
                incheckpos += SECTOR_SIZE_MODE2;
                inqueuestart += SECTOR_SIZE_MODE2;
                dataavail -= SECTOR_SIZE_MODE2;
                break;
        }
    }

    if (curtypecount) {
        fseek(in, (long)curtype_in_start, SEEK_SET);
        typetally[curtype] += (unsigned)curtypecount;
        if (flush_sector_run(&inedc, (unsigned)curtype, (unsigned)curtypecount, in, out, &progress) < 0) {
            result = 1;
            goto cleanup;
        }
    }

    /* End-of-records indicator */
    write_type_count(out, 0, 0);

    /* Input file EDC (integrity check) */
    fputc((inedc >> 0) & 0xFF, out);
    fputc((inedc >> 8) & 0xFF, out);
    fputc((inedc >> 16) & 0xFF, out);
    fputc((inedc >> 24) & 0xFF, out);

    /* Show report */
    fprintf(stderr, "Literal bytes........... %10u\n", typetally[SECTOR_TYPE_LITERAL]);
    fprintf(stderr, "Mode 1 sectors.......... %10u\n", typetally[SECTOR_TYPE_MODE1]);
    fprintf(stderr, "Mode 2 form 1 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM1]);
    fprintf(stderr, "Mode 2 form 2 sectors... %10u\n", typetally[SECTOR_TYPE_MODE2_FORM2]);
    fprintf(stderr, "Encoded %lld bytes -> %ld bytes\n", (long long)intotallength, ftell(out));
    fprintf(stderr, "Done.\n");

cleanup:
    free(inputqueue);
    return result;
}

int main(int argc, char **argv) {
    FILE *fin = NULL;
    FILE *fout = NULL;
    const char *infilename;
    char *outfilename = NULL;
    int outfilename_allocated = 0;
    int result = 0;

    banner();

    /* Initialize the ECC/EDC tables */
    eccedc_init();

    /* Check command line */
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s cdimagefile [ecmfile]\n", argv[0]);
        return 1;
    }

    infilename = argv[1];

    /* Figure out what the output filename should be */
    if (argc == 3) {
        outfilename = argv[2];
    } else {
        size_t len = strlen(infilename);
        outfilename = malloc(len + 5);
        if (!outfilename) {
            fprintf(stderr, "Error: failed to allocate output filename\n");
            return 1;
        }
        outfilename_allocated = 1;
        snprintf(outfilename, len + 5, "%s.ecm", infilename);
    }

    fprintf(stderr, "Encoding %s to %s.\n", infilename, outfilename);

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

    /* Encode */
    result = ecmify(fin, fout);

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
